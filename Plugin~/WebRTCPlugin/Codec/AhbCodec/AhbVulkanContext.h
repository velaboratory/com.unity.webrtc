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

    // Render-thread drain: for every live AHB decoder, reclaim inputs whose recorded frame
    // has completed (safeFrame) and record any pending convert into Unity's command buffer
    // `cmd` (tagged with curFrame). Called once per frame from the AHB convert plugin event
    // (UnityRenderEvent.cpp), which holds Unity's current command buffer. `cmd` is a
    // VkCommandBuffer passed as void* to keep this header free of Vulkan platform headers.
    // Implemented in AhbH264DecoderFactory.cpp.
    void AhbDrainAllDecoders(void* cmd, unsigned long long curFrame, unsigned long long safeFrame);

    // Render-thread copy: copy decoder `id`'s latest converted RGBA image into Unity's
    // receive texture image `dstImage` (a VkImage as void*, already in TRANSFER_DST).
    // Returns false if the decoder is gone or has no converted frame yet. Implemented in
    // AhbH264DecoderFactory.cpp.
    bool AhbCopyDecoderInto(unsigned long long id, void* cmd, void* dstImage, unsigned int w, unsigned int h);
}
} // namespace unity
