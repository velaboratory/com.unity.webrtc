#include "pch.h"

#include <cstdlib>
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

        struct RetireCtx
        {
            AhbVkImporter* importer;
            AhbFrameImage fi;
            AImage* img;
        };
        void RetireInput(void* p)
        {
            auto* c = static_cast<RetireCtx*>(p);
            c->importer->FreeImage(c->fi);
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
            explicit AhbH264Decoder(std::unique_ptr<::webrtc::VideoDecoder> inner)
                : inner_(std::move(inner))
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
                return true;
            }

            int32_t
            Decode(const ::webrtc::EncodedImage& input, bool missing_frames, int64_t render_time_ms) override
            {
                if (mode_ < 2)
                    return inner_ ? inner_->Decode(input, missing_frames, render_time_ms) : WEBRTC_VIDEO_CODEC_ERROR;

                NativeDecode(input);
                return WEBRTC_VIDEO_CODEC_OK; // recovery is libwebrtc's own (RTP/frame buffer)
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

            // Render thread (under g_convertMutex via AhbDrainAllDecoders): reclaim completed
            // inputs, then record the latest pending convert into Unity's command buffer.
            void RenderThreadDrain(VkCommandBuffer cmd, uint64_t curFrame, uint64_t safeFrame)
            {
                convert_.Reclaim(safeFrame);
                if (!pending_.valid)
                    return;

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
                const uint32_t w = fi.width;
                const uint32_t h = fi.height;

                bool recorded = false;
                if (convert_.IsReady() && cmd != VK_NULL_HANDLE)
                {
                    auto* ctx = new RetireCtx{ &importer_, fi, img };
                    ConvertedImage ci;
                    recorded = convert_.Record(cmd, fi.image, fi.view, w, h, curFrame, &RetireInput, ctx, &ci);
                    if (!recorded)
                    {
                        delete ctx;
                        importer_.FreeImage(fi);
                        AhbMediaCodec::ReleaseImage(img);
                    }
                }
                else
                {
                    importer_.FreeImage(fi);
                    AhbMediaCodec::ReleaseImage(img);
                }
                if (recorded && (++convOk_ <= 3 || (convOk_ % 600) == 0))
                    AHB_LOG("convert #%d %ux%u", convOk_, w, h);
            }

            // Render thread (under g_convertMutex via AhbCopyDecoderInto): copy our latest
            // converted RGBA image into Unity's receive texture `dst`.
            bool CopyLatestInto(VkCommandBuffer cmd, VkImage dst, uint32_t dstW, uint32_t dstH)
            {
                VkImage src = VK_NULL_HANDLE;
                uint32_t w = 0, h = 0;
                if (!convert_.LastImage(&src, &w, &h))
                    return false;
                if (w != dstW || h != dstH || src == VK_NULL_HANDLE)
                    return false;

                VkImageMemoryBarrier toSrc = {};
                toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSrc.image = src;
                toSrc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                    nullptr, 0, nullptr, 1, &toSrc);

                VkImageCopy region = {};
                region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.extent = { w, h, 1 };
                vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                return true;
            }

        private:
            struct Pending
            {
                AhbFrameImage fi;
                AImage* img = nullptr;
                bool valid = false;
            };

            void NativeDecode(const ::webrtc::EncodedImage& input)
            {
                if (!vkReady_)
                {
                    const UnityVulkanInstance* vk = GetUnityVulkanInstance();
                    if (!vk)
                        return; // not Vulkan, or gfx device not up yet
                    if (!importer_.Init(vk->device))
                    {
                        if (!loggedNoImport_)
                            AHB_LOG("AHB import extension unavailable — native decode disabled");
                        loggedNoImport_ = true;
                        return;
                    }
                    vkReady_ = true;
                }

                int w = static_cast<int>(input._encodedWidth);
                int h = static_cast<int>(input._encodedHeight);
                if (w <= 0 || h <= 0)
                {
                    w = cfgW_;
                    h = cfgH_;
                }
                if (w <= 0 || h <= 0)
                    return; // no resolution yet (waiting for a keyframe)

                if (!codec_.IsStarted() || w != cfgW_ || h != cfgH_)
                {
                    codec_.Release();
                    if (!codec_.Configure(w, h))
                    {
                        AHB_LOG("AhbMediaCodec.Configure(%dx%d) failed", w, h);
                        return;
                    }
                    cfgW_ = w;
                    cfgH_ = h;
                }

                const int64_t ptsUs = static_cast<int64_t>(input.Timestamp()) * 1000000 / 90000;
                codec_.DecodeNal(input.data(), input.size(), ptsUs);

                AImage* img = nullptr;
                AHardwareBuffer* ahb = codec_.AcquireLatest(&img);
                if (!ahb)
                    return; // decoder warming up / no new frame this tick

                if (!importer_.EnsureConversion(ahb))
                {
                    AhbMediaCodec::ReleaseImage(img);
                    return;
                }
                AhbFrameImage fi;
                if (!importer_.ImportImage(ahb, static_cast<uint32_t>(w), static_cast<uint32_t>(h), &fi))
                {
                    AhbMediaCodec::ReleaseImage(img);
                    return;
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
                    {
                        importer_.FreeImage(pending_.fi);
                        AhbMediaCodec::ReleaseImage(pending_.img);
                    }
                    pending_.fi = fi;
                    pending_.img = img;
                    pending_.valid = true;
                }

                // Deliver a frame carrying our id (no pixels) so the renderer copies our
                // converted image; the rtp timestamp advances so each frame is shown.
                if (realCallback_)
                {
                    auto buf = rtc::make_ref_counted<AhbDisplayBuffer>(reinterpret_cast<uint64_t>(this), w, h);
                    ::webrtc::VideoFrame frame = ::webrtc::VideoFrame::Builder()
                                                     .set_video_frame_buffer(buf)
                                                     .set_timestamp_rtp(input.Timestamp())
                                                     .set_timestamp_us(ptsUs)
                                                     .build();
                    realCallback_->Decoded(frame);
                }
            }

            void NativeRelease()
            {
                {
                    std::lock_guard<std::mutex> lock(g_convertMutex);
                    g_liveDecoders.erase(this);
                    registered_ = false;
                    if (pending_.valid)
                    {
                        importer_.FreeImage(pending_.fi);
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
            }

            std::unique_ptr<::webrtc::VideoDecoder> inner_; // delegate, only used in mode<2
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
            int mode_ = 3;
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
                std::vector<::webrtc::SdpVideoFormat> out;
                if (!android_)
                    return out;
                for (const auto& format : android_->GetSupportedFormats())
                {
                    if (format.name == "H264")
                        out.push_back(format);
                }
                return out;
            }

            std::unique_ptr<::webrtc::VideoDecoder>
            CreateVideoDecoder(const ::webrtc::SdpVideoFormat& format) override
            {
                AHB_LOG("CreateVideoDecoder(%s) -> AhbH264", format.name.c_str());
                if (!android_)
                    return nullptr;
                // The inner decoder is created but only configured/used as the mode<2
                // fallback; in the native path the decoder drops it (no extra session).
                return std::make_unique<AhbH264Decoder>(android_->CreateVideoDecoder(format));
            }

        private:
            std::unique_ptr<::webrtc::VideoDecoderFactory> android_;
        };

    } // namespace

    std::unique_ptr<VideoDecoderFactory> CreateAhbH264DecoderFactory()
    {
        return std::make_unique<AhbH264DecoderFactory>();
    }

    void AhbDrainAllDecoders(void* cmd, unsigned long long curFrame, unsigned long long safeFrame)
    {
        VkCommandBuffer vkCmd = reinterpret_cast<VkCommandBuffer>(cmd);
        std::lock_guard<std::mutex> lock(g_convertMutex);
        for (AhbH264Decoder* d : g_liveDecoders)
            d->RenderThreadDrain(vkCmd, curFrame, safeFrame);
    }

    bool AhbCopyDecoderInto(unsigned long long id, void* cmd, void* dstImage, unsigned int w, unsigned int h)
    {
        AhbH264Decoder* d = reinterpret_cast<AhbH264Decoder*>(static_cast<uintptr_t>(id));
        VkCommandBuffer vkCmd = reinterpret_cast<VkCommandBuffer>(cmd);
        VkImage dst = reinterpret_cast<VkImage>(dstImage);
        std::lock_guard<std::mutex> lock(g_convertMutex);
        if (g_liveDecoders.find(d) == g_liveDecoders.end())
            return false;
        return d->CopyLatestInto(vkCmd, dst, w, h);
    }

} // namespace webrtc
} // namespace unity
