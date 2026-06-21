#pragma once

#include <cstdint>

#include <api/video/i420_buffer.h>
#include <api/video/video_frame_buffer.h>

namespace unity
{
namespace webrtc
{
    // A kNative VideoFrameBuffer that carries no pixels — only the id (the AhbH264Decoder*
    // as a uint64) of the decoder that produced this frame. Our decoder swaps it into each
    // delivered VideoFrame so the receive renderer can find the decoder's zero-copy RGBA
    // image (via the AHB convert pass) and copy it into Unity's texture, instead of the CPU
    // ToI420 path. In this pipeline only our decoder emits kNative buffers, so the renderer
    // can treat any kNative buffer as one of these.
    //
    // ToI420() is a mandatory fallback (libwebrtc may call it from stats/adapters); it
    // returns a black frame — the real pixels travel via the GPU copy, not through here.
    class AhbDisplayBuffer : public ::webrtc::VideoFrameBuffer
    {
    public:
        AhbDisplayBuffer(uint64_t decoderId, int width, int height)
            : m_decoderId(decoderId)
            , m_width(width)
            , m_height(height)
        {
        }

        Type type() const override { return Type::kNative; }
        int width() const override { return m_width; }
        int height() const override { return m_height; }

        rtc::scoped_refptr<::webrtc::I420BufferInterface> ToI420() override
        {
            rtc::scoped_refptr<::webrtc::I420Buffer> black = ::webrtc::I420Buffer::Create(m_width, m_height);
            ::webrtc::I420Buffer::SetBlack(black.get());
            return black;
        }

        uint64_t DecoderId() const { return m_decoderId; }

    private:
        uint64_t m_decoderId;
        int m_width;
        int m_height;
    };

} // namespace webrtc
} // namespace unity
