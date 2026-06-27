#include "pch.h"

#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>

#include <android/log.h>
#include <sys/system_properties.h>

#include <vulkan/vulkan.h>

#include <IUnityGraphicsVulkan.h>
#include <api/make_ref_counted.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_decoder.h>
#include <modules/video_coding/include/video_error_codes.h>

#include "AhbConvertPass.h"
#include "AhbDecodePipeline.h"
#include "AhbDisplayBuffer.h"
#include "AhbH264DecoderFactory.h"
#include "AhbMediaCodec.h"
#include "AhbVulkanContext.h"
#include "Android/AndroidCodecFactoryHelper.h"

#define AHB_LOG(...) __android_log_print(ANDROID_LOG_INFO, "AhbH264", __VA_ARGS__)

extern "C"
{
    struct AImage;
}

namespace unity
{
namespace webrtc
{
    namespace
    {
        class AhbH264Decoder;

        // Guards the live-decoder registry AND every decoder's pending/convert state. The
        // render-thread convert/copy and the decode-thread stash both take it; teardown
        // takes it to deregister, so a decoder can't be freed while the render thread uses it.
        std::mutex g_convertMutex;
        std::set<AhbH264Decoder*> g_liveDecoders;

        // rendererId -> decoder ptr. The renderer records this from each delivered frame's
        // AhbDisplayBuffer (once per stream); C# pauses a stream's decode by rendererId
        // (visibility) and we route it to the decoder. Guarded by g_convertMutex.
        std::map<uint32_t, uint64_t> g_rendererToDecoder;

        // Frees when a convert's frame completes: only releases the AImage back to the
        // AImageReader (the VkImage is cached in the importer and reused, not freed).
        struct RetireCtx
        {
            AImage* img;
        };
        void RetireInput(void* p)
        {
            auto* c = static_cast<RetireCtx*>(p);
            AhbMediaCodec::ReleaseImage(c->img);
            delete c;
        }

        // debug.ahb.mode: <2 => delegate fallback (stock MediaCodec, CPU display — a debug
        // safety hatch); >=2 (default 3) => our native zero-copy decoder (NDK MediaCodec ->
        // AHB -> compute -> Unity texture), the production path. Latched at Configure.
        int ReadAhbMode()
        {
            char buf[PROP_VALUE_MAX] = { 0 };
            if (__system_property_get("debug.ahb.mode", buf) > 0)
                return atoi(buf);
            return 3;
        }

        // The real zero-copy H.264 decoder: it sits inside libwebrtc's receive pipeline (so
        // NACK/PLI recovery is libwebrtc's own), decodes via NDK AMediaCodec into an
        // AHardwareBuffer, imports it as a ycbcr VkImage, and delivers a kNative frame whose
        // pixels reach Unity's texture via the convert + copy recorded into Unity's frame.
        // No stock-decoder delegate runs in the production path => one decode session/stream.
        class AhbH264Decoder : public ::webrtc::VideoDecoder
        {
        public:
            AhbH264Decoder(std::unique_ptr<::webrtc::VideoDecoder> inner, std::string mime)
                : inner_(std::move(inner))
                , mime_(std::move(mime))
                , mode_(ReadAhbMode())
            {
            }

            ~AhbH264Decoder() override { NativeRelease(); }

            bool Configure(const Settings& settings) override
            {
                mode_ = ReadAhbMode();
                if (mode_ < 2)
                {
                    AHB_LOG("Configure: DELEGATE fallback (debug, mode=%d)", mode_);
                    bool ok = inner_ && inner_->Configure(settings);
                    if (ok && realCallback_)
                        inner_->RegisterDecodeCompleteCallback(realCallback_);
                    return ok;
                }
                AHB_LOG("Configure: native zero-copy decoder (mode=%d)", mode_);
                inner_.reset(); // no stock-decoder session in the native path
                m_keyFrameRequired = true; // stock: a freshly (re)inited decoder needs a keyframe first
                return true;
            }

            int32_t
            Decode(const ::webrtc::EncodedImage& input, bool missing_frames, int64_t render_time_ms) override
            {
                if (mode_ < 2)
                    return inner_ ? inner_->Decode(input, missing_frames, render_time_ms) : WEBRTC_VIDEO_CODEC_ERROR;

                // Off-screen pause REMOVED: every stream decodes and displays continuously, always.
                // (The pause/resume path caused decode+convert load to spike the moment a board came
                // into view and forced a keyframe wait on resume; a constant load is more predictable
                // and it never actually freed the HW decode session anyway.)

                // Codec-agnostic recovery (H264/VP9/AV1, DD or not — deliberately ignores `missing_frames`,
                // which never fires for AV1 without the DD). Two parts:
                //   1) after a (re)init, require a keyframe — drop deltas as NO_OUTPUT until one arrives;
                //   2) on a decode failure we SELF-RESET (below) and return ERROR. IMPORTANT: this M116
                //      VideoReceiveStream2 does NOT Release()+InitDecode() the decoder on ERROR — it only
                //      asks the sender for a keyframe. So we must drop the HW codec ourselves; the next
                //      Decode's !IsStarted() branch reconfigures it and the keyframe restarts decoding.
                if (m_keyFrameRequired)
                {
                    if (input._frameType != ::webrtc::VideoFrameType::kVideoFrameKey)
                        return WEBRTC_VIDEO_CODEC_NO_OUTPUT; // stock: "key frame required first"
                    m_keyFrameRequired = false;
                    AHB_LOG("Decode: keyframe in -> decoding (dec=%llu)",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this)));
                }

                if (!NativeDecode(input))
                {
                    // Self-reset: libwebrtc won't reinit us on ERROR (see above), so drop the HW codec
                    // (m_started -> false; the next Decode's !IsStarted() branch reconfigures) and re-require
                    // a keyframe so deltas stall until the one we request lands. Without this a hard-faulted
                    // codec (e.g. framework ResourceManager reclaim) is re-fed forever -> permanent black board.
                    codec_.Release();
                    m_keyFrameRequired = true;
                    AHB_LOG("Decode: NativeDecode failed -> reset codec + require keyframe dec=%llu",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this)));
                    return WEBRTC_VIDEO_CODEC_ERROR;
                }
                return WEBRTC_VIDEO_CODEC_OK;
            }

            int32_t RegisterDecodeCompleteCallback(::webrtc::DecodedImageCallback* callback) override
            {
                realCallback_ = callback;
                if (mode_ < 2 && inner_)
                    return inner_->RegisterDecodeCompleteCallback(callback);
                return WEBRTC_VIDEO_CODEC_OK;
            }

            int32_t Release() override
            {
                NativeRelease();
                return inner_ ? inner_->Release() : WEBRTC_VIDEO_CODEC_OK;
            }

            DecoderInfo GetDecoderInfo() const override
            {
                if (mode_ < 2 && inner_)
                {
                    DecoderInfo info = inner_->GetDecoderInfo();
                    info.implementation_name = "AhbH264-delegate(" + info.implementation_name + ")";
                    return info;
                }
                DecoderInfo info;
                info.implementation_name = "AhbH264-zerocopy";
                info.is_hardware_accelerated = true;
                return info;
            }

            // Render thread (under g_convertMutex via AhbConvertDecoderInto): reclaim
            // completed inputs, then convert the latest pending frame DIRECTLY into Unity's
            // receive texture `dstImage` (already transitioned to GENERAL) — one GPU pass,
            // no slot, no copy.
            void ConvertPendingInto(VkCommandBuffer cmd, VkImage dstImage, uint64_t curFrame, uint64_t safeFrame)
            {
                convert_.Reclaim(safeFrame);
                if (!pending_.valid)
                {
                    // HOLD-LAST: no new decoded frame this tick (e.g. the large IDR hasn't finished decoding
                    // on its own tick, so AcquireLatest returned null). The render thread still calls us with
                    // the last decoderId — repaint the last frame so the UAV receive texture isn't left
                    // CLEARED (alpha 0 => the board flashes transparent for one frame each GOP). On a starved
                    // tick no new frame was decoded, so the last frame's AHB isn't recycled and its cached
                    // VkImage is still valid; pass no retire (the importer owns that VkImage).
                    if (haveLastFi_ && convert_.IsReady() && cmd != VK_NULL_HANDLE)
                    {
                        if (convert_.RecordInto(cmd, lastFi_.image, lastFi_.view, dstImage, lastFi_.width,
                                lastFi_.height, curFrame, nullptr, nullptr) &&
                            (++holdLast_ <= 5 || (holdLast_ % 120) == 0))
                            AHB_LOG("HOLD-LAST repaint #%d %ux%u (starved tick)", holdLast_, lastFi_.width,
                                lastFi_.height);
                    }
                    return;
                }

                if (!convert_.IsReady())
                {
                    const UnityVulkanInstance* vk = GetUnityVulkanInstance();
                    VkSampler sampler = importer_.YcbcrSampler();
                    if (vk && sampler != VK_NULL_HANDLE)
                        convert_.Init(vk->device, vk->physicalDevice, vk->queueFamilyIndex, vk->graphicsQueue, sampler);
                }

                AhbFrameImage fi = pending_.fi;
                AImage* img = pending_.img;
                pending_.valid = false;

                bool recorded = false;
                if (convert_.IsReady() && cmd != VK_NULL_HANDLE)
                {
                    auto* ctx = new RetireCtx{ img };
                    recorded = convert_.RecordInto(
                        cmd, fi.image, fi.view, dstImage, fi.width, fi.height, curFrame, &RetireInput, ctx);
                    if (!recorded)
                    {
                        delete ctx;
                        AhbMediaCodec::ReleaseImage(img);
                    }
                }
                else
                {
                    AhbMediaCodec::ReleaseImage(img);
                }
                if (recorded)
                {
                    lastFi_ = fi;      // remember for HOLD-LAST repaints (the VkImage is cached in the importer)
                    haveLastFi_ = true;
                    if (++convOk_ <= 3 || (convOk_ % 600) == 0)
                        AHB_LOG("convert #%d %ux%u", convOk_, fi.width, fi.height);
                }
            }

        private:
            struct Pending
            {
                AhbFrameImage fi;
                AImage* img = nullptr;
                bool valid = false;
            };

            bool NativeDecode(const ::webrtc::EncodedImage& input)
            {
                if (!vkReady_)
                {
                    const UnityVulkanInstance* vk = GetUnityVulkanInstance();
                    if (!vk)
                        return true; // not Vulkan / gfx not up yet — not a decode failure
                    if (!importer_.Init(vk->device))
                    {
                        if (!loggedNoImport_)
                            AHB_LOG("AHB import extension unavailable — native decode disabled");
                        loggedNoImport_ = true;
                        return true; // AHB unavailable — don't drive libwebrtc into a reset loop
                    }
                    vkReady_ = true;
                }

                int w = static_cast<int>(input._encodedWidth);
                int h = static_cast<int>(input._encodedHeight);
                const bool haveInputRes = (w > 0 && h > 0);
                if (!haveInputRes)
                {
                    // Some receive paths (notably AV1, H265) never populate _encodedWidth/Height. Seed
                    // the decoder with the last-known or a default size; the MediaCodec then reports the
                    // true size via INFO_OUTPUT_FORMAT_CHANGED and resizes its AImageReader to match
                    // (we read codec_.Width()/Height() for the imported frame below).
                    w = cfgW_ > 0 ? cfgW_ : 1280;
                    h = cfgH_ > 0 ? cfgH_ : 720;
                }

                if (!codec_.IsStarted() || (haveInputRes && (w != cfgW_ || h != cfgH_)))
                {
                    codec_.Release();
                    if (!codec_.Configure(w, h, mime_.c_str()))
                    {
                        AHB_LOG("AhbMediaCodec.Configure(%s %dx%d) failed", mime_.c_str(), w, h);
                        return false; // can't (re)create the codec -> ERROR -> libwebrtc resets + re-keys
                    }
                    cfgW_ = w;
                    cfgH_ = h;
                }

                const int64_t ptsUs = static_cast<int64_t>(input.Timestamp()) * 1000000 / 90000;
                if (!codec_.DecodeNal(input.data(), input.size(), ptsUs))
                    // Decoder couldn't take the input (no HW buffers / falling behind) or hard-faulted (e.g.
                    // the framework ResourceManager reclaimed it). Return failure; Decode() turns this into a
                    // self-reset (release codec + require keyframe) + ERROR. We never silently drop/self-conceal.
                    return false;

                AImage* img = nullptr;
                AHardwareBuffer* ahb = codec_.AcquireLatest(&img);
                if (!ahb)
                    return true; // decoder warming up / no new frame this tick — not a failure

                // Frame dimensions: the input's exact encoded size when present (no codec padding);
                // otherwise (AV1/H265) the MediaCodec's reported size, learned on format change.
                int dw = haveInputRes ? w : codec_.Width();
                int dh = haveInputRes ? h : codec_.Height();
                if (dw <= 0 || dh <= 0)
                {
                    dw = w;
                    dh = h;
                }

                if (!importer_.EnsureConversion(ahb))
                {
                    AhbMediaCodec::ReleaseImage(img);
                    return true; // display-side (ycbcr) setup — the frame DID decode, not a reset case
                }
                AhbFrameImage fi;
                if (!importer_.ImportImage(ahb, static_cast<uint32_t>(dw), static_cast<uint32_t>(dh), &fi))
                {
                    AhbMediaCodec::ReleaseImage(img);
                    return true; // display-side import — the frame DID decode
                }

                // Stash for the render-thread convert.
                {
                    std::lock_guard<std::mutex> lock(g_convertMutex);
                    if (!registered_)
                    {
                        g_liveDecoders.insert(this);
                        registered_ = true;
                    }
                    if (pending_.valid)
                        AhbMediaCodec::ReleaseImage(pending_.img); // drop the unconverted frame
                    pending_.fi = fi;
                    pending_.img = img;
                    pending_.valid = true;
                }

                // Deliver a frame carrying our id (no pixels) so the renderer copies our
                // converted image; the rtp timestamp advances so each frame is shown.
                if (realCallback_)
                {
                    auto buf = rtc::make_ref_counted<AhbDisplayBuffer>(reinterpret_cast<uint64_t>(this), dw, dh);
                    ::webrtc::VideoFrame frame = ::webrtc::VideoFrame::Builder()
                                                     .set_video_frame_buffer(buf)
                                                     .set_timestamp_rtp(input.Timestamp())
                                                     .set_timestamp_us(ptsUs)
                                                     .build();
                    realCallback_->Decoded(frame);
                }
                return true;
            }

            void NativeRelease()
            {
                {
                    std::lock_guard<std::mutex> lock(g_convertMutex);
                    g_liveDecoders.erase(this);
                    registered_ = false;
                    if (pending_.valid)
                    {
                        AhbMediaCodec::ReleaseImage(pending_.img);
                        pending_.valid = false;
                    }
                    convert_.Free(); // vkDeviceWaitIdle + retire all in-flight inputs
                }
                codec_.Release();
                importer_.Shutdown();
                vkReady_ = false;
                cfgW_ = 0;
                cfgH_ = 0;
                m_keyFrameRequired = true; // a reinit after release must wait for a keyframe (stock)
            }

            std::unique_ptr<::webrtc::VideoDecoder> inner_; // delegate, only used in mode<2
            std::string mime_;                              // AMediaCodec mime for this codec (video/avc, video/x-vnd.on2.vp9, ...)
            ::webrtc::DecodedImageCallback* realCallback_ = nullptr;
            AhbMediaCodec codec_;
            AhbVkImporter importer_;
            AhbConvertPass convert_;
            Pending pending_;
            bool vkReady_ = false;
            bool loggedNoImport_ = false;
            bool registered_ = false;
            int cfgW_ = 0;
            int cfgH_ = 0;
            int convOk_ = 0;
            int holdLast_ = 0;          // HOLD-LAST repaints (should fire ~1/GOP — the starved IDR tick)
            AhbFrameImage lastFi_ = {}; // last converted frame; repainted on a starved tick (no new frame)
            bool haveLastFi_ = false;
            int mode_ = 3;

        private:
            // Matches libwebrtc stock AndroidVideoDecoder's keyFrameRequired: true on (re)init, makes us
            // drop deltas (NO_OUTPUT) until a keyframe is decoded, then cleared. (decode thread only)
            bool m_keyFrameRequired = true;
        };

        class AhbH264DecoderFactory : public ::webrtc::VideoDecoderFactory
        {
        public:
            AhbH264DecoderFactory()
                : android_(CreateAndroidDecoderFactory())
            {
                AHB_LOG("AhbH264DecoderFactory created (android delegate=%p)", static_cast<void*>(android_.get()));
            }

            std::vector<::webrtc::SdpVideoFormat> GetSupportedFormats() const override
            {
                // Start from what the platform HW decoder factory advertises (H264 Constrained Baseline,
                // VP9, AV1). We do NOT add VP8: the Quest has no HW VP8 decoder, and routing it through our
                // AHB MediaCodec produces a broken chroma plane (green). VP8 is left to libwebrtc's internal
                // libvpx decoder (kInternalImpl) + the standard (non-zero-copy) Texture2D upload — its I420
                // output can't go through the AHB zero-copy compute display anyway. The per-track display
                // switch (a Texture2D when the decoder isn't delivering native AHB frames) is what makes that
                // work. (H265 is also absent: this libwebrtc build has no HEVC codec support at all.)
                auto formats = android_ ? android_->GetSupportedFormats() : std::vector<::webrtc::SdpVideoFormat>();

                // Add H264 High + Constrained High. The platform Java factory (HardwareVideoDecoderFactory)
                // only marks H264 High-capable when the decoder NAME starts with "OMX.qcom."/"OMX.Exynos." —
                // a legacy check that misses the Quest's Codec2 decoder "c2.qti.avc.decoder", so it
                // advertises Constrained Baseline (42e01f) ONLY. But the Quest's HW AVC decoder DOES decode
                // High / Constrained High / Main (verified on-device: MediaCodecList reports
                // c2.qti.avc.decoder hardwareAccelerated, profiles Baseline/CB/Main/High/ConstrainedHigh to
                // Level 6.2). Without these advertised, a browser HARDWARE H.264 encoder — e.g. Chrome/macOS
                // VideoToolbox, which emits High/Constrained-High — negotiates to nothing here and shows no
                // video on Quest. The AHB pipeline is profile-agnostic (it maps "H264" -> "video/avc" and
                // the MediaCodec reads the real profile from the SPS), and level-asymmetry-allowed=1 keeps
                // the advertised level (5.2) from capping a sender that runs higher.
                for (const char* plid : { "640c34" /*Constrained High, 5.2*/, "640034" /*High, 5.2*/ })
                {
                    ::webrtc::SdpVideoFormat f(
                        "H264",
                        { { "profile-level-id", plid },
                          { "level-asymmetry-allowed", "1" },
                          { "packetization-mode", "1" } });
                    bool present = false;
                    for (const auto& g : formats)
                        if (g.IsSameCodec(f)) { present = true; break; }
                    if (present)
                        continue;
                    AHB_LOG("GetSupportedFormats: + H264 %s (Quest HW decodes it; platform factory omitted it)", plid);
                    formats.push_back(f);
                }
                return formats;
            }

            std::unique_ptr<::webrtc::VideoDecoder>
            CreateVideoDecoder(const ::webrtc::SdpVideoFormat& format) override
            {
                if (!android_)
                    return nullptr;
                // Drive the zero-copy AHB pipeline (MediaCodec -> AImageReader -> Vulkan compute) for any
                // codec we can map to a MediaCodec mime. The AHB output path is codec-agnostic, so VP8/VP9/
                // HEVC/AV1 zero-copy exactly like H.264 — and, crucially, they produce the kNative
                // AhbDisplayBuffer the receive-display path needs. A stock-decoder I420 frame would be
                // skipped by the zero-copy display branch and render TRANSPARENT (the VP9 bug).
                const char* mime = MimeForCodec(format.name);
                if (mime)
                {
                    AHB_LOG("CreateVideoDecoder(%s) -> AHB zero-copy (mime=%s)", format.name.c_str(), mime);
                    return std::make_unique<AhbH264Decoder>(android_->CreateVideoDecoder(format), mime);
                }
                // No AHB mime for this codec — hand it to the stock platform decoder (won't zero-copy
                // display, but better than nothing for an unforeseen codec).
                AHB_LOG("CreateVideoDecoder(%s) -> stock platform decoder (no AHB mime)", format.name.c_str());
                return android_->CreateVideoDecoder(format);
            }

        private:
            // SDP codec name -> Android MediaCodec mime for the codecs we drive through the zero-copy AHB
            // pipeline, or nullptr otherwise. Only the codecs the wrapped HW factory advertises reach here
            // (GetSupportedFormats), so each mapped mime is one the device's MediaCodec actually supports.
            // VP8 is intentionally absent: no HW VP8 on Quest, so it stays on libvpx + the standard upload.
            static const char* MimeForCodec(const std::string& name)
            {
                if (name == "H264") return "video/avc";
                if (name == "VP9")  return "video/x-vnd.on2.vp9";
                if (name == "AV1")  return "video/av01";
                return nullptr;
            }

            std::unique_ptr<::webrtc::VideoDecoderFactory> android_;
        };

    } // namespace

    std::unique_ptr<VideoDecoderFactory> CreateAhbH264DecoderFactory()
    {
        return std::make_unique<AhbH264DecoderFactory>();
    }

    // The renderer records which decoder feeds it (once per stream, from the kNative
    // frame's id) so C# can pause that decoder by rendererId.
    void AhbMapRenderer(unsigned int rendererId, unsigned long long decoderId)
    {
        std::lock_guard<std::mutex> lock(g_convertMutex);
        g_rendererToDecoder[rendererId] = decoderId;
        AHB_LOG("map renderer %u -> decoder %llu", rendererId, decoderId);
    }

    // C# visibility hook — now a NO-OP. Every stream decodes and displays continuously regardless of
    // on/off-screen state. Kept as an exported symbol so existing C# callers still link cleanly.
    void AhbSetStreamVisible(unsigned int /*rendererId*/, int /*visible*/) {}

    bool AhbConvertDecoderInto(
        unsigned long long id, void* cmd, void* dstImage, unsigned long long curFrame, unsigned long long safeFrame)
    {
        AhbH264Decoder* d = reinterpret_cast<AhbH264Decoder*>(static_cast<uintptr_t>(id));
        VkCommandBuffer vkCmd = reinterpret_cast<VkCommandBuffer>(cmd);
        VkImage dst = reinterpret_cast<VkImage>(dstImage);
        std::lock_guard<std::mutex> lock(g_convertMutex);
        if (g_liveDecoders.find(d) == g_liveDecoders.end())
            return false;
        d->ConvertPendingInto(vkCmd, dst, curFrame, safeFrame);
        return true;
    }

} // namespace webrtc
} // namespace unity
