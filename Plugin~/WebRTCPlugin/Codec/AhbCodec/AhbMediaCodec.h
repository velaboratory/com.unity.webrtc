#pragma once

#include <cstddef>
#include <cstdint>

extern "C"
{
    struct AMediaCodec;
    struct AImageReader;
    struct AImage;
    struct ANativeWindow;
    struct AHardwareBuffer;
}

namespace unity
{
namespace webrtc
{
    // NDK video decoder (AMediaCodec) that renders decoded frames straight into
    // AHardwareBuffer-backed images via an AImageReader — fully native (no Java, no
    // GL/EGL). The AHBs are then imported as VkImages (AhbVkImporter) for zero-copy.
    // Codec-agnostic: the `mime` passed to Configure selects the underlying MediaCodec
    // (video/avc, video/x-vnd.on2.vp9, video/hevc, video/av01, ...); the AHB output path
    // is identical for all of them.
    class AhbMediaCodec
    {
    public:
        AhbMediaCodec() = default;
        ~AhbMediaCodec();
        AhbMediaCodec(const AhbMediaCodec&) = delete;
        AhbMediaCodec& operator=(const AhbMediaCodec&) = delete;

        // `mime` is an Android MediaCodec mime type, e.g. "video/avc" / "video/x-vnd.on2.vp9".
        bool Configure(int width, int height, const char* mime);

        // Queue one access unit / compressed frame and pump decoder output to the AImageReader.
        bool DecodeNal(const uint8_t* data, size_t size, int64_t ptsUs);

        // Latest decoded frame's AHB, or nullptr. The returned AImage owns the AHB and
        // MUST be released via ReleaseImage() once the imported VkImage is done with it.
        AHardwareBuffer* AcquireLatest(AImage** outImage);
        static void ReleaseImage(AImage* image);

        void Release();

        int Width() const { return m_width; }
        int Height() const { return m_height; }
        bool IsStarted() const { return m_started; }

    private:
        // Pumps decoder output to the AImageReader. Returns false if the codec returned a
        // hard error (e.g. the framework ResourceManager reclaimed it under load) — the
        // caller must tear it down and reconfigure rather than keep polling a dead codec.
        bool PumpOutput();

        AMediaCodec* m_codec = nullptr;
        AImageReader* m_reader = nullptr;
        ANativeWindow* m_window = nullptr; // owned by m_reader; do not release separately
        bool m_started = false;
        int m_width = 0;
        int m_height = 0;
    };

} // namespace webrtc
} // namespace unity
