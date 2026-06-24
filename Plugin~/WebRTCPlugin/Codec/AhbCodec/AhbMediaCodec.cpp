#include "pch.h"

#include <cstring>

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>

#include "AhbMediaCodec.h"

#define AHB_LOG(...) __android_log_print(ANDROID_LOG_INFO, "AhbH264", __VA_ARGS__)

namespace unity
{
namespace webrtc
{
    AhbMediaCodec::~AhbMediaCodec() { Release(); }

    namespace
    {
        // Create an AImageReader (GPU-sampled AHBs the decoder renders into, zero copy) + its window.
        // maxImages 8 covers the convert ring (4) + the stashed pending frame + an acquire transient.
        bool MakeReader(int width, int height, AImageReader** outReader, ANativeWindow** outWindow)
        {
            *outReader = nullptr;
            *outWindow = nullptr;
            AImageReader* reader = nullptr;
            media_status_t st = AImageReader_newWithUsage(
                width, height, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 8, &reader);
            if (st != AMEDIA_OK || !reader)
            {
                AHB_LOG("AImageReader_newWithUsage(%dx%d) failed (%d)", width, height, static_cast<int>(st));
                return false;
            }
            ANativeWindow* window = nullptr;
            if (AImageReader_getWindow(reader, &window) != AMEDIA_OK || !window)
            {
                AHB_LOG("AImageReader_getWindow failed");
                AImageReader_delete(reader);
                return false;
            }
            *outReader = reader;
            *outWindow = window;
            return true;
        }
    } // namespace

    bool AhbMediaCodec::Configure(int width, int height, const char* mime)
    {
        m_width = width;
        m_height = height;
        if (!mime || !mime[0])
            mime = "video/avc";

        if (!MakeReader(width, height, &m_reader, &m_window))
            return false;

        m_codec = AMediaCodec_createDecoderByType(mime);
        if (!m_codec)
        {
            AHB_LOG("AMediaCodec_createDecoderByType(%s) failed", mime);
            return false;
        }

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
        AMediaFormat_setInt32(fmt, "low-latency", 1);
        media_status_t cs = AMediaCodec_configure(m_codec, fmt, m_window, nullptr, 0);
        AMediaFormat_delete(fmt);
        if (cs != AMEDIA_OK)
        {
            AHB_LOG("AMediaCodec_configure failed (%d)", static_cast<int>(cs));
            return false;
        }
        if (AMediaCodec_start(m_codec) != AMEDIA_OK)
        {
            AHB_LOG("AMediaCodec_start failed");
            return false;
        }
        m_started = true;
        AHB_LOG("AhbMediaCodec up: %s %dx%d (NDK AMediaCodec -> AImageReader)", mime, width, height);
        return true;
    }

    bool AhbMediaCodec::DecodeNal(const uint8_t* data, size_t size, int64_t ptsUs)
    {
        if (!m_started || !m_codec)
            return false;

        ssize_t inIdx = AMediaCodec_dequeueInputBuffer(m_codec, 0);
        if (inIdx >= 0)
        {
            size_t bufSize = 0;
            uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, static_cast<size_t>(inIdx), &bufSize);
            if (buf && data && size > 0 && size <= bufSize)
            {
                std::memcpy(buf, data, size);
                AMediaCodec_queueInputBuffer(
                    m_codec, static_cast<size_t>(inIdx), 0, size, static_cast<uint64_t>(ptsUs), 0);
            }
            else
            {
                AMediaCodec_queueInputBuffer(m_codec, static_cast<size_t>(inIdx), 0, 0, 0, 0);
            }
        }

        PumpOutput();
        return true;
    }

    void AhbMediaCodec::PumpOutput()
    {
        AMediaCodecBufferInfo info;
        for (;;)
        {
            ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(m_codec, &info, 0);
            if (outIdx >= 0)
            {
                // render = true -> the frame is composited into the AImageReader's AHB.
                AMediaCodec_releaseOutputBuffer(m_codec, static_cast<size_t>(outIdx), true);
            }
            else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
            {
                // The decoder now reports the true frame size. For codecs whose WebRTC receive path
                // doesn't set _encodedWidth (AV1, H265) we configured with a seed, so resize the
                // AImageReader to the real display dimensions by swapping the codec's output surface
                // (AMediaCodec_setOutputSurface keeps decode state — no reconfigure, no lost frame).
                AMediaFormat* fmt = AMediaCodec_getOutputFormat(m_codec);
                if (fmt)
                {
                    int32_t rw = 0, rh = 0;
                    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &rw);
                    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &rh);
                    int32_t cl = 0, cr = 0, ct = 0, cb = 0; // prefer the crop (visible) rect when present
                    if (AMediaFormat_getInt32(fmt, "crop-left", &cl) && AMediaFormat_getInt32(fmt, "crop-right", &cr) &&
                        AMediaFormat_getInt32(fmt, "crop-top", &ct) && AMediaFormat_getInt32(fmt, "crop-bottom", &cb) &&
                        cr > cl && cb > ct)
                    {
                        rw = cr - cl + 1;
                        rh = cb - ct + 1;
                    }
                    AMediaFormat_delete(fmt);
                    if (rw > 0 && rh > 0 && (rw != m_width || rh != m_height))
                    {
                        AImageReader* newReader = nullptr;
                        ANativeWindow* newWindow = nullptr;
                        if (MakeReader(rw, rh, &newReader, &newWindow))
                        {
                            if (AMediaCodec_setOutputSurface(m_codec, newWindow) == AMEDIA_OK)
                            {
                                if (m_reader)
                                    AImageReader_delete(m_reader); // codec now renders into newReader
                                m_reader = newReader;
                                m_window = newWindow;
                                m_width = rw;
                                m_height = rh;
                                AHB_LOG("output format -> %dx%d (AImageReader resized)", rw, rh);
                            }
                            else
                            {
                                AImageReader_delete(newReader);
                                AHB_LOG("AMediaCodec_setOutputSurface(%dx%d) failed", rw, rh);
                            }
                        }
                    }
                }
                continue;
            }
            else if (outIdx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            {
                continue;
            }
            else
            {
                break; // AMEDIACODEC_INFO_TRY_AGAIN_LATER
            }
        }
    }

    AHardwareBuffer* AhbMediaCodec::AcquireLatest(AImage** outImage)
    {
        if (!m_reader || !outImage)
            return nullptr;
        AImage* img = nullptr;
        if (AImageReader_acquireLatestImage(m_reader, &img) != AMEDIA_OK || !img)
            return nullptr;
        AHardwareBuffer* ahb = nullptr;
        if (AImage_getHardwareBuffer(img, &ahb) != AMEDIA_OK || !ahb)
        {
            AImage_delete(img);
            return nullptr;
        }
        *outImage = img;
        return ahb;
    }

    void AhbMediaCodec::ReleaseImage(AImage* image)
    {
        if (image)
            AImage_delete(image);
    }

    void AhbMediaCodec::Release()
    {
        if (m_codec)
        {
            if (m_started)
                AMediaCodec_stop(m_codec);
            AMediaCodec_delete(m_codec);
            m_codec = nullptr;
        }
        // m_window is owned by m_reader; deleting the reader frees it.
        m_window = nullptr;
        if (m_reader)
        {
            AImageReader_delete(m_reader);
            m_reader = nullptr;
        }
        m_started = false;
    }

} // namespace webrtc
} // namespace unity
