#pragma once

// Unity's Vulkan device/instance/queue, captured at graphics-device init in
// UnityRenderEvent.cpp. Returns nullptr until init, or on a non-Vulkan renderer.
// Lets the AhbH264 decoder reach Unity's VkDevice (for AHB import) and the graphics
// queue (for the convert pass, via AccessQueue).
struct UnityVulkanInstance;

namespace unity
{
namespace webrtc
{
    const UnityVulkanInstance* GetUnityVulkanInstance();

    // Render-thread convert: for decoder `id`, reclaim completed inputs (safeFrame) and
    // convert its latest pending frame DIRECTLY into Unity's receive texture image
    // `dstImage` (a VkImage as void*, already transitioned to GENERAL), recorded into
    // Unity's command buffer `cmd` and tagged with `curFrame`. Returns false if the decoder
    // is gone. void* params keep this header free of Vulkan platform headers. Implemented in
    // AhbH264DecoderFactory.cpp.
    bool AhbConvertDecoderInto(
        unsigned long long id, void* cmd, void* dstImage, unsigned long long curFrame, unsigned long long safeFrame);

    // Render thread: record which decoder (by id from the kNative frame) feeds rendererId,
    // so C# can pause that decoder by rendererId on visibility changes.
    void AhbMapRenderer(unsigned int rendererId, unsigned long long decoderId);

    // C# visibility hook: pause (visible==0) / resume the decode for the stream shown on
    // rendererId. Paused = the decoder stops feeding MediaCodec (idle HW decoder).
    void AhbSetStreamVisible(unsigned int rendererId, int visible);
}
} // namespace unity
