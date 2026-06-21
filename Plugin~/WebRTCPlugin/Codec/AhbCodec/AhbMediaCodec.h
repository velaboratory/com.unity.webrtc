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
    // NDK H.264 decoder (AMediaCodec) that renders decoded frames straight into
    // AHardwareBuffer-backed images via an AImageReader — fully native (no Java, no
    // GL/EGL). The AHBs are then imported as VkImages (AhbVkImporter) for zero-copy.
    class AhbMediaCodec
    {
    public:
        AhbMediaCodec() = default;
        ~AhbMediaCodec();
        AhbMediaCodec(const AhbMediaCodec&) = delete;
        AhbMediaCodec& operator=(const AhbMediaCodec&) = delete;

        bool Configure(int width, int height);

        // Queue one Annex-B access unit and pump decoder output to the AImageReader.
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
        void PumpOutput();

        AMediaCodec* m_codec = nullptr;
        AImageReader* m_reader = nullptr;
        ANativeWindow* m_window = nullptr; // owned by m_reader; do not release separately
        bool m_started = false;
        int m_width = 0;
        int m_height = 0;
    };

} // namespace webrtc
} // namespace unity
