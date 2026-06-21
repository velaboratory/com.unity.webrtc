#include "pch.h"

#include <android/log.h>

#include "AhbConvertPass.h"
#include "GraphicsDevice/Vulkan/LoadVulkanFunctions.h"

#define AHB_LOG(...) __android_log_print(ANDROID_LOG_INFO, "AhbH264", __VA_ARGS__)

namespace unity
{
namespace webrtc
{
    namespace
    {
        // YUV(ycbcr)->RGBA8 compute shader, compiled from ycbcr_to_rgba.comp by the NDK
        // glslc (see that file's header for the regen command).
        const uint32_t kYcbcrToRgbaSpv[] =
#include "ycbcr_to_rgba.spv.inc"
            ;

        uint32_t DivUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
    } // namespace

    bool AhbConvertPass::BuildPipeline(VkSampler ycbcrSampler)
    {
        VkSampler immutable = ycbcrSampler;
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[0].pImmutableSamplers = &immutable;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo dslci = {};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 2;
        dslci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &m_setLayout) != VK_SUCCESS)
        {
            AHB_LOG("vkCreateDescriptorSetLayout failed");
            return false;
        }

        VkPipelineLayoutCreateInfo plci = {};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &m_setLayout;
        if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipeLayout) != VK_SUCCESS)
        {
            AHB_LOG("vkCreatePipelineLayout failed");
            return false;
        }

        VkShaderModuleCreateInfo smci = {};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(kYcbcrToRgbaSpv);
        smci.pCode = kYcbcrToRgbaSpv;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &smci, nullptr, &module) != VK_SUCCESS)
        {
            AHB_LOG("vkCreateShaderModule (ycbcr->rgba) failed");
            return false;
        }

        VkComputePipelineCreateInfo cpci = {};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = "main";
        cpci.layout = m_pipeLayout;
        VkResult pr = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_pipeline);
        vkDestroyShaderModule(m_device, module, nullptr);
        if (pr != VK_SUCCESS)
        {
            AHB_LOG("vkCreateComputePipelines failed (%d)", static_cast<int>(pr));
            return false;
        }

        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = kRing;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = kRing;
        VkDescriptorPoolCreateInfo dpci = {};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = kRing;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_descPool) != VK_SUCCESS)
        {
            AHB_LOG("vkCreateDescriptorPool failed");
            return false;
        }

        for (int i = 0; i < kRing; i++)
        {
            VkDescriptorSetAllocateInfo dsai = {};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = m_descPool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &m_setLayout;
            if (vkAllocateDescriptorSets(m_device, &dsai, &m_slots[i].set) != VK_SUCCESS)
            {
                AHB_LOG("vkAllocateDescriptorSets failed");
                return false;
            }
        }
        return true;
    }

    bool AhbConvertPass::Init(
        VkDevice device, VkPhysicalDevice phys, uint32_t queueFamily, VkQueue queue, VkSampler ycbcrSampler)
    {
        m_device = device;
        m_phys = phys;
        m_queueFamily = queueFamily;
        m_queue = queue;
        if (ycbcrSampler == VK_NULL_HANDLE)
        {
            AHB_LOG("AhbConvertPass::Init with null ycbcr sampler");
            return false;
        }
        return BuildPipeline(ycbcrSampler);
    }

    VkImageView AhbConvertPass::EnsureDstView(VkImage dstImage)
    {
        if (dstImage == m_dstImage && m_dstView != VK_NULL_HANDLE)
            return m_dstView;
        if (m_dstView != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, m_dstView, nullptr);
        m_dstView = VK_NULL_HANDLE;
        m_dstImage = VK_NULL_HANDLE;

        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = dstImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &vci, nullptr, &m_dstView) != VK_SUCCESS)
        {
            AHB_LOG("EnsureDstView vkCreateImageView failed");
            m_dstView = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        m_dstImage = dstImage;
        return m_dstView;
    }

    void AhbConvertPass::RetireSlot(Slot& s)
    {
        if (s.inFlight && s.retire)
            s.retire(s.retireCtx);
        s.inFlight = false;
        s.retire = nullptr;
        s.retireCtx = nullptr;
    }

    bool AhbConvertPass::RecordInto(VkCommandBuffer cmd, VkImage srcImage, VkImageView srcView, VkImage dstImage,
        uint32_t w, uint32_t h, uint64_t frameNumber, AhbRetireFn retire, void* retireCtx)
    {
        if (!IsReady() || cmd == VK_NULL_HANDLE || srcView == VK_NULL_HANDLE || dstImage == VK_NULL_HANDLE)
            return false;
        VkImageView dstView = EnsureDstView(dstImage);
        if (dstView == VK_NULL_HANDLE)
            return false;

        Slot& s = m_slots[m_next];
        if (s.inFlight) // GPU more than kRing frames behind — skip rather than overwrite
            return false;
        m_next = (m_next + 1) % kRing;

        VkDescriptorImageInfo srcInfo = {};
        srcInfo.sampler = VK_NULL_HANDLE; // immutable in the layout
        srcInfo.imageView = srcView;
        srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo dstInfo = {};
        dstInfo.imageView = dstView;
        dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s.set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &srcInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s.set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dstInfo;
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

        // src (imported AHB) UNDEFINED -> SHADER_READ_ONLY (content lives in the AHB).
        VkImageMemoryBarrier toRead = {};
        toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toRead.srcAccessMask = 0;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = srcImage;
        toRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
            0, nullptr, 1, &toRead);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 0, 1, &s.set, 0, nullptr);
        vkCmdDispatch(cmd, DivUp(w, 8), DivUp(h, 8), 1);

        s.inFlight = true;
        s.recordedFrame = frameNumber;
        s.retire = retire;
        s.retireCtx = retireCtx;
        return true;
    }

    void AhbConvertPass::Reclaim(uint64_t safeFrameNumber)
    {
        for (int i = 0; i < kRing; i++)
        {
            if (m_slots[i].inFlight && m_slots[i].recordedFrame <= safeFrameNumber)
                RetireSlot(m_slots[i]);
        }
    }

    void AhbConvertPass::Free()
    {
        if (m_device == VK_NULL_HANDLE)
            return;
        vkDeviceWaitIdle(m_device);
        for (int i = 0; i < kRing; i++)
            RetireSlot(m_slots[i]);
        if (m_dstView != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, m_dstView, nullptr);
        if (m_descPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
        if (m_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
        if (m_pipeLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(m_device, m_pipeLayout, nullptr);
        if (m_setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_dstView = VK_NULL_HANDLE;
        m_dstImage = VK_NULL_HANDLE;
        m_descPool = VK_NULL_HANDLE;
        m_pipeline = VK_NULL_HANDLE;
        m_pipeLayout = VK_NULL_HANDLE;
        m_setLayout = VK_NULL_HANDLE;
        m_next = 0;
    }

} // namespace webrtc
} // namespace unity
