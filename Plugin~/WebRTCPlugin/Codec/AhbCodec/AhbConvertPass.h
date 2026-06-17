#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace unity
{
namespace webrtc
{
    // The RGBA8 result of one YUV->RGBA convert. Owned by AhbConvertPass (ring). Becomes
    // valid once Unity submits the frame it was recorded into (tracked by frame number).
    struct ConvertedImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Called when a convert's input is safe to free — Unity's safeFrameNumber has passed
    // the frame the convert was recorded into, so the GPU is done sampling the imported
    // AHB. Releases the imported VkImage + the AImage back to the AImageReader.
    typedef void (*AhbRetireFn)(void* ctx);

    // A Vulkan compute pass that samples a vendor-YUV (ycbcr) VkImage through the
    // importer's immutable ycbcr sampler and writes a plain RGBA8 image.
    //
    // CRITICAL: it does NOT submit anything. Record() records barriers + dispatch into
    // UNITY'S current command buffer (obtained on the render thread via
    // CommandRecordingState); Unity submits it with its own frame. Submitting our own work
    // to Unity's graphics queue out-of-band crashes the VR compositor (proven on-device),
    // so this is the only safe path. Input lifetime is tracked by Unity frame numbers:
    // Reclaim(safeFrameNumber) frees inputs whose recorded frame has completed.
    //
    // Init() must run AFTER the importer's EnsureConversion (it bakes the immutable ycbcr
    // sampler into the descriptor-set-layout). Record()/Reclaim() run on the render thread.
    class AhbConvertPass
    {
    public:
        bool Init(VkDevice device, VkPhysicalDevice phys, uint32_t queueFamily, VkQueue queue, VkSampler ycbcrSampler);

        // Record a convert of srcView (ycbcr) into Unity's command buffer `cmd`, targeting
        // the next ring slot. srcImage is needed to transition the imported AHB image
        // (initial layout UNDEFINED) to SHADER_READ. `frameNumber` is Unity's
        // currentFrameNumber; the input is retained (via retire/retireCtx) until a later
        // safeFrameNumber passes it. Returns true if recorded (false if no free slot).
        bool Record(VkCommandBuffer cmd, VkImage srcImage, VkImageView srcView, uint32_t w, uint32_t h,
            uint64_t frameNumber, AhbRetireFn retire, void* retireCtx, ConvertedImage* out);

        // Free inputs whose recorded frame has completed (recordedFrame <= safeFrameNumber).
        void Reclaim(uint64_t safeFrameNumber);

        // The most recently recorded RGBA8 output (the display source). Valid once at least
        // one convert has been recorded; the consumer should read it a frame later (it is
        // GPU-complete by then). Returns false if nothing recorded yet.
        bool LastImage(VkImage* outImage, uint32_t* outW, uint32_t* outH) const;

        void Free();

        bool IsReady() const { return m_pipeline != VK_NULL_HANDLE; }

    private:
        static const int kRing = 4;

        struct Slot
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE; // RGBA8 view (storage for dispatch; Unity makes its own)
            VkDescriptorSet set = VK_NULL_HANDLE;
            bool inFlight = false;
            bool everConverted = false; // false => image layout is still UNDEFINED
            uint64_t recordedFrame = 0;
            AhbRetireFn retire = nullptr;
            void* retireCtx = nullptr;
        };

        bool BuildPipeline(VkSampler ycbcrSampler);
        bool EnsureOutputs(uint32_t w, uint32_t h);
        bool AllocSlotImage(Slot& s, uint32_t w, uint32_t h);
        void RetireSlot(Slot& s);
        void FreeSlotImage(Slot& s);
        bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags, uint32_t* outIndex);

        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_phys = VK_NULL_HANDLE;
        uint32_t m_queueFamily = 0;
        VkQueue m_queue = VK_NULL_HANDLE;

        VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkDescriptorPool m_descPool = VK_NULL_HANDLE;

        Slot m_slots[kRing] = {};
        uint32_t m_outW = 0;
        uint32_t m_outH = 0;
        int m_next = 0;
        int m_lastSlot = -1; // most recently recorded slot (the display source)
    };

} // namespace webrtc
} // namespace unity
