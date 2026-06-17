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

    bool AhbConvertPass::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags, uint32_t* outIndex)
    {
        VkPhysicalDeviceMemoryProperties props = {};
        vkGetPhysicalDeviceMemoryProperties(m_phys, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        {
            if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            {
                *outIndex = i;
                return true;
            }
        }
        return false;
    }

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

    bool AhbConvertPass::AllocSlotImage(Slot& s, uint32_t w, uint32_t h)
    {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { w, h, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &s.image) != VK_SUCCESS)
        {
            AHB_LOG("AllocSlotImage vkCreateImage failed");
            return false;
        }

        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(m_device, s.image, &req);
        uint32_t typeIndex = 0;
        if (!FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
        {
            AHB_LOG("AllocSlotImage no device-local memory type");
            return false;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(m_device, &mai, nullptr, &s.memory) != VK_SUCCESS)
        {
            AHB_LOG("AllocSlotImage vkAllocateMemory failed");
            return false;
        }
        if (vkBindImageMemory(m_device, s.image, s.memory, 0) != VK_SUCCESS)
        {
            AHB_LOG("AllocSlotImage vkBindImageMemory failed");
            return false;
        }

        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &vci, nullptr, &s.view) != VK_SUCCESS)
        {
            AHB_LOG("AllocSlotImage vkCreateImageView failed");
            return false;
        }

        s.everConverted = false;
        return true;
    }

    void AhbConvertPass::RetireSlot(Slot& s)
    {
        if (s.inFlight && s.retire)
            s.retire(s.retireCtx);
        s.inFlight = false;
        s.retire = nullptr;
        s.retireCtx = nullptr;
    }

    void AhbConvertPass::FreeSlotImage(Slot& s)
    {
        if (s.view != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, s.view, nullptr);
        if (s.image != VK_NULL_HANDLE)
            vkDestroyImage(m_device, s.image, nullptr);
        if (s.memory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, s.memory, nullptr);
        s.view = VK_NULL_HANDLE;
        s.image = VK_NULL_HANDLE;
        s.memory = VK_NULL_HANDLE;
    }

    bool AhbConvertPass::EnsureOutputs(uint32_t w, uint32_t h)
    {
        if (m_outW == w && m_outH == h && m_slots[0].image != VK_NULL_HANDLE)
            return true;

        // Resolution change (or first use): drain + realloc. Caller guarantees the render
        // thread, and Reclaim has freed completed inputs; for a resolution change we wait
        // the device idle so no pending Unity submit still references the old images.
        if (m_slots[0].image != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
            for (int i = 0; i < kRing; i++)
            {
                RetireSlot(m_slots[i]);
                FreeSlotImage(m_slots[i]);
            }
        }

        m_outW = w;
        m_outH = h;
        for (int i = 0; i < kRing; i++)
        {
            if (!AllocSlotImage(m_slots[i], w, h))
            {
                AHB_LOG("EnsureOutputs failed at slot %d (%ux%u)", i, w, h);
                return false;
            }
        }
        return true;
    }

    bool AhbConvertPass::Record(VkCommandBuffer cmd, VkImage srcImage, VkImageView srcView, uint32_t w, uint32_t h,
        uint64_t frameNumber, AhbRetireFn retire, void* retireCtx, ConvertedImage* out)
    {
        if (!IsReady() || cmd == VK_NULL_HANDLE || srcView == VK_NULL_HANDLE)
            return false;
        if (!EnsureOutputs(w, h))
            return false;

        Slot& s = m_slots[m_next];
        // The next slot must already be reclaimed (its prior submit completed). If not, the
        // GPU is more than kRing frames behind — skip this convert rather than overwrite.
        if (s.inFlight)
            return false;
        m_next = (m_next + 1) % kRing;

        VkDescriptorImageInfo srcInfo = {};
        srcInfo.sampler = VK_NULL_HANDLE; // immutable in the layout
        srcInfo.imageView = srcView;
        srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo dstInfo = {};
        dstInfo.imageView = s.view;
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

        // src (imported AHB) UNDEFINED -> SHADER_READ_ONLY (content lives in the AHB,
        // independent of Vulkan layout, so UNDEFINED preserves it).
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

        VkImageMemoryBarrier toGeneral = {};
        toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral.srcAccessMask = 0;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        // The dispatch overwrites the entire image, so we never need the old contents — and
        // the slot may currently be in SHADER_READ (last convert) or TRANSFER_SRC (after the
        // display copy). UNDEFINED covers both.
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = s.image;
        toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkImageMemoryBarrier pre[2] = { toRead, toGeneral };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
            0, nullptr, 2, pre);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 0, 1, &s.set, 0, nullptr);
        vkCmdDispatch(cmd, DivUp(w, 8), DivUp(h, 8), 1);

        VkImageMemoryBarrier toSampled = {};
        toSampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSampled.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSampled.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSampled.image = s.image;
        toSampled.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
            nullptr, 0, nullptr, 1, &toSampled);

        s.inFlight = true;
        s.everConverted = true;
        s.recordedFrame = frameNumber;
        s.retire = retire;
        s.retireCtx = retireCtx;
        m_lastSlot = (m_next + kRing - 1) % kRing; // the slot we just used

        if (out)
        {
            out->image = s.image;
            out->view = s.view;
            out->width = w;
            out->height = h;
        }
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

    bool AhbConvertPass::LastImage(VkImage* outImage, uint32_t* outW, uint32_t* outH) const
    {
        if (m_lastSlot < 0 || m_slots[m_lastSlot].image == VK_NULL_HANDLE)
            return false;
        *outImage = m_slots[m_lastSlot].image;
        *outW = m_outW;
        *outH = m_outH;
        return true;
    }

    void AhbConvertPass::Free()
    {
        if (m_device == VK_NULL_HANDLE)
            return;
        // Teardown: ensure no pending Unity submit still references our images/inputs.
        vkDeviceWaitIdle(m_device);
        for (int i = 0; i < kRing; i++)
        {
            RetireSlot(m_slots[i]);
            FreeSlotImage(m_slots[i]);
            m_slots[i].set = VK_NULL_HANDLE; // freed with the pool
        }
        if (m_descPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
        if (m_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
        if (m_pipeLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(m_device, m_pipeLayout, nullptr);
        if (m_setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_descPool = VK_NULL_HANDLE;
        m_pipeline = VK_NULL_HANDLE;
        m_pipeLayout = VK_NULL_HANDLE;
        m_setLayout = VK_NULL_HANDLE;
        m_outW = 0;
        m_outH = 0;
        m_next = 0;
    }

} // namespace webrtc
} // namespace unity
