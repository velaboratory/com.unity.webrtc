#include "pch.h"

#include <api/video_codecs/video_codec.h>
#include <media/engine/internal_decoder_factory.h>
#include <modules/video_coding/include/video_error_codes.h>

#include "Codec/CreateVideoCodecFactory.h"
#include "GraphicsDevice/GraphicsUtility.h"
#include "ProfilerMarkerFactory.h"
#include "ScopedProfiler.h"
#include "UnityVideoDecoderFactory.h"

namespace unity
{
namespace webrtc
{
    class UnityVideoDecoder : public VideoDecoder
    {
    public:
        UnityVideoDecoder(std::unique_ptr<VideoDecoder> decoder, ProfilerMarkerFactory* profiler)
            : decoder_(std::move(decoder))
            , profiler_(profiler)
            , marker_(nullptr)
            , profilerThread_(nullptr)
        {
            if (profiler)
                marker_ = profiler->CreateMarker(
                    "UnityVideoDecoder.Decode", kUnityProfilerCategoryOther, kUnityProfilerMarkerFlagDefault, 0);
        }
        ~UnityVideoDecoder() override { }

        bool Configure(const Settings& settings) override
        {
            bool result = decoder_->Configure(settings);
            if (result && !profilerThread_)
            {
                std::stringstream ss;
                ss << "Decoder ";
                ss
                    << (decoder_->GetDecoderInfo().implementation_name.empty()
                            ? "VideoDecoder"
                            : decoder_->GetDecoderInfo().implementation_name);
                ss << "(" << CodecTypeToPayloadString(settings.codec_type()) << ")";
                profilerThread_ = profiler_->CreateScopedProfilerThread("WebRTC", ss.str().c_str());
            }

            return result;
        }
        int32_t Decode(const EncodedImage& input_image, bool missing_frames, int64_t render_time_ms) override
        {
            int32_t result;
            {
                std::unique_ptr<const ScopedProfiler> profiler;
                if (profiler_)
                    profiler = profiler_->CreateScopedProfiler(*marker_);
                result = decoder_->Decode(input_image, missing_frames, render_time_ms);
            }
            return result;
        }
        int32_t RegisterDecodeCompleteCallback(DecodedImageCallback* callback) override
        {
            return decoder_->RegisterDecodeCompleteCallback(callback);
        }
        int32_t Release() override { return decoder_->Release(); }
        DecoderInfo GetDecoderInfo() const override { return decoder_->GetDecoderInfo(); }

    private:
        std::unique_ptr<VideoDecoder> decoder_;
        ProfilerMarkerFactory* profiler_;
        const UnityProfilerMarkerDesc* marker_;
        std::unique_ptr<const ScopedProfilerThread> profilerThread_;
    };

    UnityVideoDecoderFactory::UnityVideoDecoderFactory(IGraphicsDevice* gfxDevice, ProfilerMarkerFactory* profiler)
        : profiler_(profiler)
        , factories_()
    {
        const std::vector<std::string> arrayImpl = {
            kAhbH264Impl, kInternalImpl, kNvCodecImpl, kAndroidMediaCodecImpl, kVideoToolboxImpl
        };

        for (auto impl : arrayImpl)
        {
            auto factory = CreateVideoDecoderFactory(impl, gfxDevice, profiler);
            if (factory)
                factories_.emplace(impl, factory);
        }
    }

    UnityVideoDecoderFactory::~UnityVideoDecoderFactory() = default;

    std::vector<webrtc::SdpVideoFormat> UnityVideoDecoderFactory::GetSupportedFormats() const
    {
        auto formats = GetSupportedFormatsInFactories(factories_);

        // On macOS/iOS the platform decoder factory (VideoToolbox) advertises ONLY H.264 Constrained
        // Baseline (42e01f) + Constrained High (640c1f) — even though VideoToolbox decodes plain High fine.
        // Browser HARDWARE H.264 encoders (Chrome/macOS VideoToolbox) only emit Baseline/Main/High, NEVER
        // Constrained Baseline, so without High advertised here a hardware-encoded stream negotiates to
        // nothing in the Editor. Advertise High (640034) when VideoToolbox H.264 is present; CreateVideoDecoder
        // routes an unmatched High request to VideoToolbox (it decodes the High bitstream regardless of the
        // SDP profile). On Android the AHB factory already advertises High, so this dedups to a no-op there.
        if (factories_.count(kVideoToolboxImpl) > 0)
        {
            webrtc::SdpVideoFormat high(
                "H264",
                { { "profile-level-id", "640034" },
                  { "level-asymmetry-allowed", "1" },
                  { "packetization-mode", "1" } });
            bool present = false;
            for (const auto& f : formats)
                if (f.IsSameCodec(high)) { present = true; break; }
            if (!present)
            {
                high.parameters.emplace(kSdpKeyNameCodecImpl, std::string(kVideoToolboxImpl));
                formats.push_back(high);
            }
        }
        return formats;
    }

    std::unique_ptr<webrtc::VideoDecoder>
    UnityVideoDecoderFactory::CreateVideoDecoder(const webrtc::SdpVideoFormat& format)
    {
        VideoDecoderFactory* factory = FindCodecFactory(factories_, format);
        if (!factory)
        {
            // The High (640034) we advertise above has no sub-factory that lists it (the objc VideoToolbox
            // factory exposes only CB+CH), but VideoToolbox decodes High fine — route unmatched H.264 to it.
            auto it = factories_.find(kVideoToolboxImpl);
            if (it != factories_.end() && format.name == "H264")
                factory = it->second.get();
        }
        if (!factory)
            return nullptr;
        auto decoder = factory->CreateVideoDecoder(format);
        if (!profiler_)
            return decoder;

        // Use Unity Profiler for measuring decoding process.
        return std::make_unique<UnityVideoDecoder>(std::move(decoder), profiler_);
    }

} // namespace webrtc
} // namespace unity
