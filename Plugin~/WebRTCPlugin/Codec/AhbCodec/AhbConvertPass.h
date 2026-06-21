#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace unity
{
namespace webrtc
{
    // Called when a convert's input is safe to free — Unity's safeFrameNumber has passed
    // the frame it was recorded into, so the GPU is done sampling the imported AHB.
    typedef void (*AhbRetireFn)(void* ctx);

    // A Vulkan compute pass: samples a vendor-YUV (ycbcr) VkImage through the importer's
    // immutable ycbcr sampler and writes RGBA8 DIRECTLY into Unity's receive RenderTexture
    // (a storage image). One GPU pass per frame — no intermediate slot, no copy (that
    // doubled the per-stream GPU cost and was the perf bottleneck).
    //
    // Records into Unity's command buffer (never an out-of-band submit). Input lifetime is
    // tracked by Unity frame numbers: Reclaim(safeFrameNumber) frees inputs whose recorded
    // frame has completed. A small ring of descriptor sets covers in-flight frames.
    //
    // Init() must run AFTER the importer's EnsureConversion (the immutable ycbcr sampler is
    // baked into the descriptor-set-layout). RecordInto()/Reclaim() run on the render thread.
    class AhbConvertPass
    {
    public:
        bool Init(VkDevice device, VkPhysicalDevice phys, uint32_t queueFamily, VkQueue queue, VkSampler ycbcrSampler);

        // Record a convert of srcView (ycbcr) into dstImage (Unity's RenderTexture, already
        // transitioned to GENERAL by the caller). srcImage is needed to transition the
        // imported AHB image to SHADER_READ. `frameNumber` = Unity's currentFrameNumber;
        // the input is retained (via retire/retireCtx) until a later safeFrameNumber passes.
        bool RecordInto(VkCommandBuffer cmd, VkImage srcImage, VkImageView srcView, VkImage dstImage, uint32_t w,
            uint32_t h, uint64_t frameNumber, AhbRetireFn retire, void* retireCtx);

        // Free inputs whose recorded frame has completed (recordedFrame <= safeFrameNumber).
        void Reclaim(uint64_t safeFrameNumber);

        void Free();

        bool IsReady() const { return m_pipeline != VK_NULL_HANDLE; }

    private:
        static const int kRing = 4;

        struct Slot
        {
            VkDescriptorSet set = VK_NULL_HANDLE;
            bool inFlight = false;
            uint64_t recordedFrame = 0;
            AhbRetireFn retire = nullptr;
            void* retireCtx = nullptr;
        };

        bool BuildPipeline(VkSampler ycbcrSampler);
        VkImageView EnsureDstView(VkImage dstImage);
        void RetireSlot(Slot& s);

        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_phys = VK_NULL_HANDLE;
        uint32_t m_queueFamily = 0;
        VkQueue m_queue = VK_NULL_HANDLE;

        VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkDescriptorPool m_descPool = VK_NULL_HANDLE;

        // Cached storage view of Unity's receive texture (stable per stream; rebuilt if the
        // texture is recreated on a resolution change).
        VkImage m_dstImage = VK_NULL_HANDLE;
        VkImageView m_dstView = VK_NULL_HANDLE;

        Slot m_slots[kRing] = {};
        int m_next = 0;
    };

} // namespace webrtc
} // namespace unity
