#pragma once

#include <mutex>

#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>
#include <third_party/libyuv/include/libyuv.h>

#include "WebRTCPlugin.h"

namespace unity
{
namespace webrtc
{

    using namespace ::webrtc;

    class UnityVideoRenderer : public rtc::VideoSinkInterface<::webrtc::VideoFrame>
    {
    public:
        UnityVideoRenderer(uint32_t id, DelegateVideoFrameResize callback, bool needFlipVertical);
        ~UnityVideoRenderer() override;
        void OnFrame(const ::webrtc::VideoFrame& frame) override;

        uint32_t GetId();
        rtc::scoped_refptr<VideoFrameBuffer> GetFrameBuffer();
        void SetFrameBuffer(rtc::scoped_refptr<VideoFrameBuffer> buffer, int64_t timestamp);

        // Whether the most recent decoded frame was a kNative (zero-copy AHB) buffer. Lets C# pick the
        // zero-copy RenderTexture display for HW/AHB decoders vs the standard Texture2D upload for
        // software decoders (libvpx VP8) whose I420 frames the AHB compute display can't consume.
        bool LastFrameWasNative() const { return m_lastFrameNative.load(std::memory_order_relaxed); }

#if UNITY_ANDROID
        // The AHB decoder feeding this renderer (0 until its first zero-copy frame). On a starved tick
        // (no new frame this Unity frame) the render thread repaints this decoder's last frame so the
        // receive texture isn't left cleared/transparent.
        uint64_t LastAhbDecoderId() const { return m_lastMappedDecoder; }
#endif

        // used in UnityRenderingExtEventUpdateTexture
        // called on RenderThread
        void* ConvertVideoFrameToTextureAndWriteToBuffer(int width, int height, libyuv::FourCC format);

    private:
        uint32_t m_id;
        std::mutex m_mutex;
        std::vector<uint8_t> tempBuffer;
        rtc::scoped_refptr<webrtc::VideoFrameBuffer> m_frameBuffer;
        int64_t m_last_renderered_timestamp;
        std::atomic<int64_t> m_timestamp;
        DelegateVideoFrameResize m_callback;
        bool m_needFlipVertical;
        std::atomic<bool> m_lastFrameNative{ false }; // set in OnFrame; read lock-free by C# at resize
#if UNITY_ANDROID
        uint64_t m_lastMappedDecoder = 0; // AHB decoder->renderer map recorded once per stream
#endif
    };

} // end namespace webrtc
} // end namespace unity
