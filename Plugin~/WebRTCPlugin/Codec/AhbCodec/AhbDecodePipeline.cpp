#include "pch.h"

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan_android.h>

#include <android/hardware_buffer.h>
#include <android/log.h>

#include "AhbDecodePipeline.h"
#include "GraphicsDevice/Vulkan/LoadVulkanFunctions.h"

#define AHB_LOG(...) __android_log_print(ANDROID_LOG_INFO, "AhbH264", __VA_ARGS__)

namespace unity
{
namespace webrtc
{
    namespace
    {
        uint32_t FirstSetBit(uint32_t mask)
        {
            for (uint32_t i = 0; i < 32; i++)
                if (mask & (1u << i))
                    return i;
            return 0;
        }

        bool QueryFormat(void* getAhbPropsPfn, VkDevice device, AHardwareBuffer* ahb,
            VkAndroidHardwareBufferFormatPropertiesANDROID* fmt, VkAndroidHardwareBufferPropertiesANDROID* props)
        {
            auto getAhbProps = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(getAhbPropsPfn);
            if (!getAhbProps || !ahb)
                return false;
            fmt->sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
            fmt->pNext = nullptr;
            props->sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
            props->pNext = fmt;
            if (getAhbProps(device, ahb, props) != VK_SUCCESS)
            {
                AHB_LOG("vkGetAndroidHardwareBufferProperties failed");
                return false;
            }
            return true;
        }
    } // namespace

    bool AhbVkImporter::Init(VkDevice device)
    {
        m_device = device;
        m_getAhbProps =
            reinterpret_cast<void*>(vkGetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
        if (!m_getAhbProps)
        {
            AHB_LOG("vkGetAndroidHardwareBufferPropertiesANDROID NULL — AHB import extension not enabled");
            return false;
        }

        // Core 1.1 names first, then the KHR aliases (1.0 device + extension).
        m_createYcbcr = reinterpret_cast<void*>(vkGetDeviceProcAddr(device, "vkCreateSamplerYcbcrConversion"));
        if (!m_createYcbcr)
            m_createYcbcr = reinterpret_cast<void*>(vkGetDeviceProcAddr(device, "vkCreateSamplerYcbcrConversionKHR"));
        m_destroyYcbcr = reinterpret_cast<void*>(vkGetDeviceProcAddr(device, "vkDestroySamplerYcbcrConversion"));
        if (!m_destroyYcbcr)
            m_destroyYcbcr = reinterpret_cast<void*>(vkGetDeviceProcAddr(device, "vkDestroySamplerYcbcrConversionKHR"));
        if (!m_createYcbcr || !m_destroyYcbcr)
            AHB_LOG("vkCreateSamplerYcbcrConversion NULL — vendor-YUV AHBs cannot be sampled");

        return true;
    }

    bool AhbVkImporter::EnsureConversion(AHardwareBuffer* ahb)
    {
        if (m_conversionReady)
            return true;

        VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps = {};
        VkAndroidHardwareBufferPropertiesANDROID ahbProps = {};
        if (!QueryFormat(m_getAhbProps, m_device, ahb, &fmtProps, &ahbProps))
            return false;

        m_isYcbcr = (fmtProps.format == VK_FORMAT_UNDEFINED);
        if (!m_isYcbcr)
        {
            // RGBA8 AHB — Unity samples it directly, no conversion needed.
            m_conversionReady = true;
            return true;
        }

        auto createYcbcr = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(m_createYcbcr);
        if (!createYcbcr)
        {
            AHB_LOG("no ycbcr conversion entry point; cannot sample vendor-YUV AHB");
            return false;
        }

        VkExternalFormatANDROID extFmt = {};
        extFmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
        extFmt.externalFormat = fmtProps.externalFormat;

        const bool linearOk =
            (fmtProps.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0;
        const VkFilter chromaFilter = linearOk ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

        VkSamplerYcbcrConversionCreateInfo cci = {};
        cci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
        cci.pNext = &extFmt;
        cci.format = VK_FORMAT_UNDEFINED;
        cci.ycbcrModel = fmtProps.suggestedYcbcrModel;
        cci.ycbcrRange = fmtProps.suggestedYcbcrRange;
        cci.components = fmtProps.samplerYcbcrConversionComponents;
        cci.xChromaOffset = fmtProps.suggestedXChromaOffset;
        cci.yChromaOffset = fmtProps.suggestedYChromaOffset;
        cci.chromaFilter = chromaFilter;
        cci.forceExplicitReconstruction = VK_FALSE;
        if (createYcbcr(m_device, &cci, nullptr, &m_conversion) != VK_SUCCESS)
        {
            AHB_LOG("vkCreateSamplerYcbcrConversion failed");
            return false;
        }

        VkSamplerYcbcrConversionInfo convInfo = {};
        convInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        convInfo.conversion = m_conversion;

        VkSamplerCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.pNext = &convInfo;
        sci.magFilter = chromaFilter;
        sci.minFilter = chromaFilter;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.unnormalizedCoordinates = VK_FALSE;
        if (vkCreateSampler(m_device, &sci, nullptr, &m_sampler) != VK_SUCCESS)
        {
            AHB_LOG("vkCreateSampler (ycbcr) failed");
            auto destroyYcbcr = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(m_destroyYcbcr);
            if (destroyYcbcr)
                destroyYcbcr(m_device, m_conversion, nullptr);
            m_conversion = VK_NULL_HANDLE;
            return false;
        }

        AHB_LOG("ycbcr conversion ready (externalFormat=%llu chromaFilter=%d)",
            static_cast<unsigned long long>(fmtProps.externalFormat), static_cast<int>(chromaFilter));
        m_conversionReady = true;
        return true;
    }

    bool AhbVkImporter::ImportMemory(
        AHardwareBuffer* ahb, VkImage image, uint64_t allocationSize, uint32_t memoryTypeBits, VkDeviceMemory* outMemory)
    {
        VkImportAndroidHardwareBufferInfoANDROID importInfo = {};
        importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        importInfo.buffer = ahb;

        VkMemoryDedicatedAllocateInfo ded = {};
        ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded.pNext = &importInfo;
        ded.image = image;

        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &ded;
        mai.allocationSize = allocationSize;
        mai.memoryTypeIndex = FirstSetBit(memoryTypeBits);

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vkAllocateMemory(m_device, &mai, nullptr, &memory) != VK_SUCCESS)
        {
            AHB_LOG("vkAllocateMemory (import AHB) failed");
            return false;
        }

        VkBindImageMemoryInfo bind = {};
        bind.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
        bind.image = image;
        bind.memory = memory;
        bind.memoryOffset = 0;
        if (vkBindImageMemory2(m_device, 1, &bind) != VK_SUCCESS)
        {
            AHB_LOG("vkBindImageMemory2 failed");
            vkFreeMemory(m_device, memory, nullptr);
            return false;
        }

        *outMemory = memory;
        return true;
    }

    bool AhbVkImporter::ImportImage(AHardwareBuffer* ahb, uint32_t width, uint32_t height, AhbFrameImage* out)
    {
        if (!out || !m_conversionReady)
            return false;
        // The AImageReader recycles a small fixed pool of AHBs; import each ONCE and reuse
        // the VkImage (it tracks the AHB's content across cycles). Avoids per-frame churn.
        auto it = m_cache.find(ahb);
        if (it != m_cache.end())
        {
            *out = it->second;
            return true;
        }
        if (!ImportNew(ahb, width, height, out))
            return false;
        m_cache[ahb] = *out;
        return true;
    }

    bool AhbVkImporter::ImportNew(AHardwareBuffer* ahb, uint32_t width, uint32_t height, AhbFrameImage* out)
    {
        VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps = {};
        VkAndroidHardwareBufferPropertiesANDROID ahbProps = {};
        if (!QueryFormat(m_getAhbProps, m_device, ahb, &fmtProps, &ahbProps))
            return false;

        const bool isYcbcr = (fmtProps.format == VK_FORMAT_UNDEFINED);

        AhbFrameImage result = {};
        result.isYcbcr = isYcbcr;
        result.width = width;
        result.height = height;

        VkExternalFormatANDROID extFmt = {};
        extFmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
        extFmt.externalFormat = fmtProps.externalFormat;

        VkExternalMemoryImageCreateInfo extImg = {};
        extImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        extImg.pNext = isYcbcr ? &extFmt : nullptr;
        extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &extImg;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = isYcbcr ? VK_FORMAT_UNDEFINED : VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { width, height, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &result.image) != VK_SUCCESS)
        {
            AHB_LOG("vkCreateImage (external AHB) failed");
            return false;
        }

        if (!ImportMemory(ahb, result.image, ahbProps.allocationSize, ahbProps.memoryTypeBits, &result.memory))
        {
            vkDestroyImage(m_device, result.image, nullptr);
            return false;
        }

        if (isYcbcr)
        {
            VkSamplerYcbcrConversionInfo convInfo = {};
            convInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
            convInfo.conversion = m_conversion;

            VkImageViewCreateInfo vci = {};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.pNext = &convInfo;
            vci.image = result.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_UNDEFINED;
            vci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(m_device, &vci, nullptr, &result.view) != VK_SUCCESS)
            {
                AHB_LOG("vkCreateImageView (ycbcr) failed");
                vkFreeMemory(m_device, result.memory, nullptr);
                vkDestroyImage(m_device, result.image, nullptr);
                return false;
            }
        }

        *out = result;
        return true;
    }

    void AhbVkImporter::FreeImage(AhbFrameImage& img)
    {
        if (img.view != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, img.view, nullptr);
        if (img.image != VK_NULL_HANDLE)
            vkDestroyImage(m_device, img.image, nullptr);
        if (img.memory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, img.memory, nullptr);
        img = AhbFrameImage{};
    }

    void AhbVkImporter::Shutdown()
    {
        for (auto& kv : m_cache)
            FreeImage(kv.second);
        m_cache.clear();
        if (m_sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_sampler, nullptr);
            m_sampler = VK_NULL_HANDLE;
        }
        if (m_conversion != VK_NULL_HANDLE)
        {
            auto destroyYcbcr = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(m_destroyYcbcr);
            if (destroyYcbcr)
                destroyYcbcr(m_device, m_conversion, nullptr);
            m_conversion = VK_NULL_HANDLE;
        }
        m_conversionReady = false;
        m_isYcbcr = false;
    }

} // namespace webrtc
} // namespace unity
