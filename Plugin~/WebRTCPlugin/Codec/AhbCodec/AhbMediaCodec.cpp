#include "pch.h"

#include <cstring>
#include <unistd.h> // usleep — reap-before-create backoff

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
        // Wait up to this long for a free input buffer before treating a frame as undecodable — matches
        // libwebrtc's stock AndroidVideoDecoder DEQUEUE_INPUT_TIMEOUT_US (500 ms). Each receive stream
        // decodes on its own thread, so this only ever blocks the single stream that's backed up.
        constexpr int64_t kDequeueInputTimeoutUs = 500000;

        // Reap-before-create retry: if opening a decode session fails at the concurrent-session cap
        // (a just-reset sibling's session hasn't reaped yet — venus_hfi_session_close is async), retry
        // up to this many times with this backoff so the slot frees first. ~8 x 50ms = 400ms max, only
        // ever on a reset right at the cap; the common case succeeds on attempt 1.
        constexpr int kConfigureRetries = 8;
        constexpr int kConfigureRetryDelayUs = 50000; // 50 ms

        // Create an AImageReader (GPU-sampled AHBs the decoder renders into, zero copy) + its window.
        // The consumer side holds up to (1 stashed pending + kRing=4 in-flight GPU converts) = 5 of these
        // at once, so the pool size minus 5 is what the decoder gets to RENDER into. At 8 the decoder had
        // only 3 (== the CCodec numClientBuffers(3) seen in logcat); under 6x1440p60 those 3 starve, the
        // decoder can't dequeue a free output buffer, and DecodeNal then drops the next NAL -> reference
        // chain breaks -> visual corruption + "QC2V4L2PollThread Unsupported input buffer" floods. 12
        // leaves the decoder 7, more than doubling its headroom. Cost ~ width*height*1.5 bytes per extra
        // buffer per stream (≈5.5 MB @1440p); keep this modest if you run many (12+) concurrent streams.
        static const int kReaderImages = 12;
        bool MakeReader(int width, int height, AImageReader** outReader, ANativeWindow** outWindow)
        {
            *outReader = nullptr;
            *outWindow = nullptr;
            AImageReader* reader = nullptr;
            media_status_t st = AImageReader_newWithUsage(
                width, height, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, kReaderImages, &reader);
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

        // Reap-before-create: a just-reset sibling decoder's HW session can linger briefly
        // (venus_hfi_session_close is async), so opening a new session right at the concurrent-session
        // cap (avcd=16) transiently fails with ENOMEM ("sessions exceeded max limit"). Erroring there
        // makes libwebrtc reset us into a cap-churn STORM at 15-16 streams. Instead, retry with a short
        // backoff so the lingering session reaps first — effectively waiting for a slot before creating
        // ours. Only a genuinely-full cap (no slot frees within the window) still fails.
        for (int attempt = 1; attempt <= kConfigureRetries; ++attempt)
        {
            m_codec = AMediaCodec_createDecoderByType(mime);
            if (m_codec)
            {
                AMediaFormat* fmt = AMediaFormat_new();
                AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
                AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
                AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
                AMediaFormat_setInt32(fmt, "low-latency", 1);
                media_status_t cs = AMediaCodec_configure(m_codec, fmt, m_window, nullptr, 0);
                AMediaFormat_delete(fmt);
                if (cs == AMEDIA_OK && AMediaCodec_start(m_codec) == AMEDIA_OK)
                {
                    m_started = true;
                    AHB_LOG("AhbMediaCodec up: %s %dx%d (attempt %d)", mime, width, height, attempt);
                    return true;
                }
                AHB_LOG("AhbMediaCodec configure/start failed (%s %dx%d, attempt %d, cs=%d) — likely the "
                        "session cap; reaping + retrying",
                    mime, width, height, attempt, static_cast<int>(cs));
                AMediaCodec_delete(m_codec); // drop the half-open codec before retrying
                m_codec = nullptr;
            }
            else
            {
                AHB_LOG("AMediaCodec_createDecoderByType(%s) failed (attempt %d)", mime, attempt);
            }
            if (attempt < kConfigureRetries)
                usleep(kConfigureRetryDelayUs); // let a just-released sibling's HW session reap, then retry
        }
        // All attempts failed: free the AImageReader/window we made up front (MakeReader, above) so a
        // wedged stream doesn't pin a slot of the (now 12-image) pool until teardown.
        m_window = nullptr; // owned by m_reader; deleting the reader frees it
        if (m_reader)
        {
            AImageReader_delete(m_reader);
            m_reader = nullptr;
        }
        AHB_LOG("AhbMediaCodec.Configure(%s %dx%d) failed after %d attempts (cap genuinely full?)",
            mime, width, height, kConfigureRetries);
        return false;
    }

    bool AhbMediaCodec::DecodeNal(const uint8_t* data, size_t size, int64_t ptsUs)
    {
        if (!m_started || !m_codec)
            return false;

        // Match stock AndroidVideoDecoder: wait ~500ms for an input buffer. If none is available the
        // decoder is falling behind / faulted — return failure so Decode() returns ERROR and libwebrtc
        // resets + re-keys. We never silently drop the frame (the old timeout-0 drop is what left AV1
        // unable to recover from loss — the decoder concealed forever instead of signalling a failure).
        ssize_t inIdx = AMediaCodec_dequeueInputBuffer(m_codec, kDequeueInputTimeoutUs);
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
        else
        {
            // No input buffer after the timeout (-1) or a hard error (e.g. ResourceManager reclaim) —
            // "decoder falling behind"; signal failure so libwebrtc tears us down and requests a keyframe.
            AHB_LOG("dequeueInputBuffer no buffer/err %zd — decoder falling behind (reset)", inIdx);
            return false;
        }

        return PumpOutput();
    }

    bool AhbMediaCodec::PumpOutput()
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
            else if (outIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            {
                break; // no output ready this pump — normal
            }
            else
            {
                // Any other negative is a hard error (media_status_t < 0): the codec was
                // released/reclaimed or faulted. Stop polling it — returning false makes the
                // caller tear down + reconfigure instead of spamming "Invalid to call at
                // Released state" every frame against a dead codec.
                AHB_LOG("dequeueOutputBuffer error %zd — codec needs reset", outIdx);
                return false;
            }
        }
        return true;
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
