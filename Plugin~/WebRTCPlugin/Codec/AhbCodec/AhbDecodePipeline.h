#pragma once

#include <cstdint>
#include <map>

#include <vulkan/vulkan.h>

struct AHardwareBuffer;

namespace unity
{
namespace webrtc
{
    // One decoder frame imported onto Unity's VkDevice with no CPU copy. For the common
    // vendor-YUV case (isYcbcr) the image carries an external (VK_FORMAT_UNDEFINED)
    // format and MUST be sampled through `view` + the importer's shared immutable ycbcr
    // sampler, which apply the YUV->RGB conversion. A compute pass then writes a plain
    // RGBA8 image Unity can display.
    struct AhbFrameImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE; // ycbcr-aware view (ycbcr path)
        bool isYcbcr = false;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Imports AHardwareBuffers from the H.264 decoder as VkImages on Unity's VkDevice.
    // The vendor-YUV conversion (VkSamplerYcbcrConversion) + its immutable VkSampler
    // depend only on the stream's external format, so they are created ONCE
    // (EnsureConversion) and shared by every frame's view + by the convert pass's
    // descriptor-set-layout. Per-frame work is just ImportImage (image+memory+view).
    // Ported from the proven velvideovk import, minus the device-intercept hack.
    class AhbVkImporter
    {
    public:
        // Loads vkGetAndroidHardwareBufferPropertiesANDROID (+ the ycbcr-conversion
        // entry points). Returns false if the AHB import extension is absent.
        bool Init(VkDevice device);

        // Inspect a representative decoder AHB and, if it is vendor-YUV, create the
        // shared conversion + immutable sampler. No-op (returns true) for RGBA8 AHBs.
        // Safe to call repeatedly; only the first call builds anything.
        bool EnsureConversion(AHardwareBuffer* ahb);

        // Import an AHB -> image(+memory)(+ycbcr view), CACHED by AHB pointer. The
        // AImageReader recycles a small fixed set of AHBs, so each is imported ONCE and the
        // VkImage reused across cycles (it always reflects the AHB's current content). Per
        // frame the caller only releases the AImage (returns the buffer to the reader); the
        // VkImages live until Shutdown. Avoids ~7 Vulkan driver calls per frame per stream.
        bool ImportImage(AHardwareBuffer* ahb, uint32_t width, uint32_t height, AhbFrameImage* out);

        // Release the cached images + the shared conversion + sampler.
        void Shutdown();

        VkSampler YcbcrSampler() const { return m_sampler; }
        bool IsYcbcr() const { return m_isYcbcr; }
        bool IsReady() const { return m_getAhbProps != nullptr; }

    private:
        bool ImportMemory(AHardwareBuffer* ahb, VkImage image, uint64_t allocationSize, uint32_t memoryTypeBits,
            VkDeviceMemory* outMemory);
        bool ImportNew(AHardwareBuffer* ahb, uint32_t width, uint32_t height, AhbFrameImage* out);
        void FreeImage(AhbFrameImage& img);

        // Imported VkImages keyed by the recycled AHB pointer (bounded by the reader's pool).
        std::map<AHardwareBuffer*, AhbFrameImage> m_cache;

        VkDevice m_device = VK_NULL_HANDLE;
        void* m_getAhbProps = nullptr;  // PFN_vkGetAndroidHardwareBufferPropertiesANDROID
        void* m_createYcbcr = nullptr;  // PFN_vkCreateSamplerYcbcrConversion(KHR)
        void* m_destroyYcbcr = nullptr; // PFN_vkDestroySamplerYcbcrConversion(KHR)

        // Shared per-stream ycbcr state.
        VkSamplerYcbcrConversion m_conversion = VK_NULL_HANDLE;
        VkSampler m_sampler = VK_NULL_HANDLE;
        bool m_isYcbcr = false;
        bool m_conversionReady = false;
    };

} // namespace webrtc
} // namespace unity
