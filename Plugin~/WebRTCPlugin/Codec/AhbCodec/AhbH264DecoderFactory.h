#pragma once

#include <memory>

#include <api/video_codecs/video_decoder_factory.h>

namespace unity
{
namespace webrtc
{
    using namespace ::webrtc;

    // Custom H.264 decoder factory for Android. It wins H.264 selection in
    // UnityVideoDecoderFactory (its impl key "AhbH264" sorts before "MediaCodec" in
    // the std::map the aggregate iterates), so libwebrtc's full receive pipeline
    // (NACK retransmit, decoder-driven PLI, congestion control, jitter/frame buffer)
    // drives OUR decoder — giving native, flawless loss recovery.
    //
    // Phase 1a: delegates actual decoding to the stock Android MediaCodec decoder
    // while logging, to prove the slot-in. Phase 1b swaps the delegate for the
    // zero-copy MediaCodec->Surface->AHB->Vulkan path.
    std::unique_ptr<VideoDecoderFactory> CreateAhbH264DecoderFactory();

} // namespace webrtc
} // namespace unity
