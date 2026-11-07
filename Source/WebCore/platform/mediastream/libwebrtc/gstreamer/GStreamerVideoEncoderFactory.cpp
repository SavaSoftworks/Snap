/*
 * Copyright (C) 2018 Metrological Group B.V.
 * Copyright (C) 2018 Igalia S.L. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"

#if USE(LIBWEBRTC) && USE(GSTREAMER)
#include "GStreamerVideoEncoderFactory.h"

#include "FloatSize.h"
#include "GStreamerRegistryScanner.h"
#include "GStreamerVideoCommon.h"
#include "GStreamerVideoFrameLibWebRTC.h"
#include "LibWebRTCUtils.h"
#include "LibWebRTCVideoFrameUtilities.h"
#include "LibWebRTCWebKitMacros.h"
#include "VideoEncoderGStreamer.h"
#include "VideoEncoderPrivateGStreamer.h"
#include "VideoEncoderScalabilityMode.h"
#include "VideoFrameGStreamer.h"
#include "webrtc/api/make_ref_counted.h"
#include "webrtc/api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "webrtc/api/video_codecs/vp9_profile.h"
#include "webrtc/media/engine/simulcast_encoder_adapter.h"
#include "webrtc/modules/video_coding/codecs/vp8/include/vp8.h"
#include "webrtc/modules/video_coding/codecs/vp9/include/vp9.h"
#include "webrtc/modules/video_coding/include/video_codec_interface.h"
#include "webrtc/modules/video_coding/include/video_error_codes.h"
#if USE(LIBWEBRTC_AV1)
#include "webrtc/api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <gst/video/video.h>
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/StdMap.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

GST_DEBUG_CATEGORY(webkit_webrtcenc_debug);
#define GST_CAT_DEFAULT webkit_webrtcenc_debug

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerVideoEncoderFactory);

class GStreamerEncodedImageBuffer : public webrtc::EncodedImageBufferInterface {
public:
    static webrtc::scoped_refptr<GStreamerEncodedImageBuffer> create(Vector<uint8_t>&& data)
    {
        return webrtc::make_ref_counted<GStreamerEncodedImageBuffer>(WTF::move(data));
    }

    const uint8_t* data() const override { return m_data.span().data(); }
    size_t size() const override { return m_data.size(); }

protected:
    explicit GStreamerEncodedImageBuffer(Vector<uint8_t>&& data)
        : m_data(WTF::move(data))
    {
    }

private:
    Vector<uint8_t> m_data;
};

static constexpr unsigned h264KeyFrameInterval = 3000;
static constexpr size_t maximumPendingFrames = 64;
static constexpr double maximumDeclaredFrameRate = 30;
static constexpr size_t measuredFrameIntervals = 5;
static constexpr size_t recentFrameIntervals = 3;
static constexpr double minimumRateFactor = 0.25;
static constexpr double maximumRateFactor = 4;
static constexpr double rateFactorStep = 0.1;

static std::vector<webrtc::SdpVideoFormat> h264EncoderFormats()
{
    std::vector<webrtc::SdpVideoFormat> formats;
    for (const char* profileLevelId : { "42001f", "42e01f", "4d001f" })
        formats.emplace_back("H264", webrtc::CodecParameterMap { { "level-asymmetry-allowed", "1" }, { "packetization-mode", "1" }, { "profile-level-id", profileLevelId } });
    return formats;
}

class LibWebRTCGStreamerVideoEncoder final : public webrtc::VideoEncoder {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(LibWebRTCGStreamerVideoEncoder);
public:
    explicit LibWebRTCGStreamerVideoEncoder(const webrtc::SdpVideoFormat& sdpVideoFormat)
        : m_sdpVideoFormat(sdpVideoFormat)
    {
        m_codecInfo.codecType = webrtc::kVideoCodecH264;
        auto packetizationMode = m_sdpVideoFormat.parameters.find("packetization-mode");
        bool singleNalUnit = packetizationMode != m_sdpVideoFormat.parameters.end() && packetizationMode->second == "0";
        m_codecInfo.codecSpecific.H264.packetization_mode = singleNalUnit ? webrtc::H264PacketizationMode::SingleNalUnit : webrtc::H264PacketizationMode::NonInterleaved;
        m_codecInfo.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
        m_codecInfo.codecSpecific.H264.base_layer_sync = false;
        m_codecInfo.codecSpecific.H264.idr_frame = false;
    }

    ~LibWebRTCGStreamerVideoEncoder()
    {
        Release();
    }

    int32_t InitEncode(const webrtc::VideoCodec* codecSettings, const webrtc::VideoEncoder::Settings&) final
    {
        if (!codecSettings || codecSettings->codecType != webrtc::kVideoCodecH264 || !codecSettings->width || !codecSettings->height)
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        if (codecSettings->numberOfSimulcastStreams > 1)
            return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;

        m_size = { static_cast<int>(codecSettings->width), static_cast<int>(codecSettings->height) };
        m_frameRate = std::clamp<double>(codecSettings->maxFramerate ? codecSettings->maxFramerate : maximumDeclaredFrameRate, 1, maximumDeclaredFrameRate);
        m_bitRate = static_cast<uint64_t>(codecSettings->startBitrate) * 1000;
        return createInternalEncoder();
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) final
    {
        m_encodedImageCallback = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() final
    {
        if (RefPtr encoder = std::exchange(m_internalEncoder, nullptr))
            encoder->close();
        m_pendingFrames.clear();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) final
    {
        m_inputFrameRate = m_framesSeen >= 2 ? parameters.framerate_fps : 0;
        m_bitRate = parameters.bitrate.get_sum_bps();
        updateRateFactor();
        applyRates();
    }

    VideoEncoder::EncoderInfo GetEncoderInfo() const final
    {
        VideoEncoder::EncoderInfo info;
        info.implementation_name = "GStreamer";
        info.is_hardware_accelerated = m_isHardwareAccelerated;
        info.has_trusted_rate_controller = false;
        info.supports_simulcast = false;
        info.requested_resolution_alignment = 2;
        info.preferred_pixel_formats = { webrtc::VideoFrameBuffer::Type::kI420 };
        info.supports_native_handle = m_acceptsDMABuf;
        return info;
    }

    int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* frameTypes) final
    {
        if (!m_internalEncoder || !m_encodedImageCallback) [[unlikely]]
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

        bool shouldGenerateKeyFrame = false;
        if (frameTypes) {
            for (auto frameType : *frameTypes) {
                if (frameType == webrtc::VideoFrameType::kVideoFrameKey)
                    shouldGenerateKeyFrame = true;
            }
        }

        IntSize frameSize { frame.width(), frame.height() };
        bool encoderScalesFrame = m_acceptsDMABuf && frame.video_frame_buffer() && frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNative;
        if (!encoderScalesFrame && frameSize != m_size) {
            GST_INFO("Input frame size changed from %dx%d to %dx%d, recreating the encoder", m_size.width(), m_size.height(), frameSize.width(), frameSize.height());
            m_size = frameSize;
            if (auto result = createInternalEncoder(); result != WEBRTC_VIDEO_CODEC_OK)
                return result;
            shouldGenerateKeyFrame = true;
        }

        auto sample = convertLibWebRTCVideoFrameToGStreamerSample(frame, m_acceptsDMABuf);
        if (!sample) [[unlikely]] {
            GST_WARNING("Unable to wrap a %dx%d frame for encoding", frame.width(), frame.height());
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        noteFrameTime(frame.timestamp_us());
        if (updateRateFactor())
            applyRates();

        m_pendingFrames.append({ frame.render_time_ms(), frame.rtp_timestamp() });
        while (m_pendingFrames.size() > maximumPendingFrames)
            m_pendingFrames.removeFirst();

        VideoFrameGStreamer::CreateOptions options;
        options.presentationSize = m_size;
        auto colorSpace = colorSpaceFromLibWebRTCVideoFrame(frame);
        auto gstVideoFrame = VideoFrameGStreamer::create(WTF::move(sample), options, colorSpace.value_or(PlatformVideoColorSpace { }));
        WebCore::VideoEncoder::RawFrame rawFrame { WTF::move(gstVideoFrame), frame.render_time_ms(), { } };
        if (m_internalEncoder->encodeSync(WTF::move(rawFrame), shouldGenerateKeyFrame))
            return WEBRTC_VIDEO_CODEC_OK;

        return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
    }

private:
    struct PendingFrame {
        int64_t renderTimeMs { 0 };
        uint32_t rtpTimestamp { 0 };
    };

    int32_t createInternalEncoder()
    {
        Release();

        StringBuilder builder;
        builder.append("avc1"_s);
        auto profileLevelId = m_sdpVideoFormat.parameters.find("profile-level-id");
        if (profileLevelId != m_sdpVideoFormat.parameters.end() && !profileLevelId->second.empty())
            builder.append('.', fromStdString(profileLevelId->second));
        auto codecName = builder.toString();

        WebCore::VideoEncoder::Config config;
        config.width = m_size.width();
        config.height = m_size.height();
        config.useAnnexB = true;
        config.bitRate = encoderBitRate();
        config.frameRate = m_frameRate;
        config.isRealtime = true;

        GST_INFO("Creating %dx%d H.264 encoder for codec %s", m_size.width(), m_size.height(), codecName.ascii().data());
        auto result = GStreamerVideoEncoder::create(codecName, config, [](auto&&) { }, [this](auto&& encodedFrame) { notifyEncodedFrame(WTF::move(encodedFrame)); });
        if (!result) {
            GST_ERROR("Unable to create a GStreamer H.264 encoder: %s", result.error().utf8().data());
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        m_internalEncoder = WTF::move(*result);
        m_internalEncoder->setKeyFrameIntervalSync(h264KeyFrameInterval);
        m_isHardwareAccelerated = m_internalEncoder->isHardwareAccelerated();
        m_acceptsDMABuf = m_internalEncoder->acceptsDMABuf();
        GST_INFO("The H.264 encoder is %shardware accelerated and %s DMA-BUF frames", m_isHardwareAccelerated ? "" : "not ", m_acceptsDMABuf ? "takes" : "does not take");
        m_encoderBitRate = 0;
        applyRates();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void noteFrameTime(int64_t timestampUs)
    {
        if (m_framesSeen < 2)
            ++m_framesSeen;
        if (!m_frameTimes.isEmpty() && timestampUs <= m_frameTimes.last()) {
            if (timestampUs < m_frameTimes.last())
                m_frameTimes.clear();
            else
                return;
        }
        m_frameTimes.append(timestampUs);
        if (m_frameTimes.size() > measuredFrameIntervals + 1)
            m_frameTimes.removeAt(0);
    }

    double measuredFrameRate() const
    {
        size_t available = m_frameTimes.isEmpty() ? 0 : m_frameTimes.size() - 1;
        if (available < recentFrameIntervals)
            return 0;
        std::array<int64_t, measuredFrameIntervals> intervals;
        auto medianInterval = [&](size_t count) {
            for (size_t index = 0; index < count; ++index) {
                size_t newer = m_frameTimes.size() - count + index;
                intervals[index] = m_frameTimes[newer] - m_frameTimes[newer - 1];
            }
            std::sort(intervals.begin(), intervals.begin() + count);
            return intervals[count / 2];
        };
        auto interval = medianInterval(recentFrameIntervals);
        if (available >= measuredFrameIntervals)
            interval = std::min(interval, medianInterval(measuredFrameIntervals));
        return interval > 0 ? 1000000.0 / interval : 0;
    }

    bool updateRateFactor()
    {
        double frameRate = std::max(m_inputFrameRate, measuredFrameRate());
        if (frameRate < 1)
            return false;
        double factor = std::clamp(m_frameRate / frameRate, minimumRateFactor, maximumRateFactor);
        if (std::abs(factor / m_rateFactor - 1) < rateFactorStep)
            return false;
        GST_DEBUG("Frames arrive at %.1f fps for a %.0f fps H.264 encoder, scaling its bitrate by %.2f", frameRate, m_frameRate, factor);
        m_rateFactor = factor;
        return true;
    }

    uint64_t encoderBitRate() const
    {
        return static_cast<uint64_t>(std::llround(m_bitRate * m_rateFactor));
    }

    void applyRates()
    {
        if (!m_internalEncoder || !m_bitRate)
            return;
        auto bitRate = encoderBitRate();
        if (bitRate == m_encoderBitRate)
            return;
        m_encoderBitRate = bitRate;
        m_internalEncoder->setRatesSync(bitRate, m_frameRate);
    }

    std::optional<uint32_t> takeRtpTimestamp(int64_t renderTimeMs)
    {
        while (!m_pendingFrames.isEmpty() && m_pendingFrames.first().renderTimeMs < renderTimeMs)
            m_pendingFrames.removeFirst();
        if (m_pendingFrames.isEmpty() || m_pendingFrames.first().renderTimeMs != renderTimeMs)
            return std::nullopt;
        return m_pendingFrames.takeFirst().rtpTimestamp;
    }

    void notifyEncodedFrame(WebCore::VideoEncoder::EncodedFrame&& frame)
    {
        if (!m_encodedImageCallback || frame.data.isEmpty()) [[unlikely]]
            return;

        auto rtpTimestamp = takeRtpTimestamp(frame.timestamp);
        if (!rtpTimestamp) {
            GST_WARNING("Dropping an encoded frame with no matching input frame at %" G_GINT64_FORMAT " ms", frame.timestamp);
            return;
        }

        webrtc::EncodedImage encodedImage;
        encodedImage.SetEncodedData(GStreamerEncodedImageBuffer::create(WTF::move(frame.data)));
        encodedImage._encodedWidth = m_size.width();
        encodedImage._encodedHeight = m_size.height();
        encodedImage.SetFrameType(frame.isKeyFrame ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta);
        encodedImage.SetRtpTimestamp(*rtpTimestamp);
        encodedImage.capture_time_ms_ = frame.timestamp;
        m_codecInfo.codecSpecific.H264.idr_frame = frame.isKeyFrame;
        m_encodedImageCallback->OnEncodedImage(encodedImage, &m_codecInfo);
    }

    webrtc::SdpVideoFormat m_sdpVideoFormat;
    webrtc::CodecSpecificInfo m_codecInfo;
    RefPtr<GStreamerVideoEncoder> m_internalEncoder;
    webrtc::EncodedImageCallback* m_encodedImageCallback { nullptr };
    IntSize m_size;
    double m_frameRate { maximumDeclaredFrameRate };
    double m_inputFrameRate { 0 };
    double m_rateFactor { 1 };
    unsigned m_framesSeen { 0 };
    uint64_t m_bitRate { 0 };
    uint64_t m_encoderBitRate { 0 };
    Vector<int64_t, measuredFrameIntervals + 1> m_frameTimes;
    bool m_isHardwareAccelerated { false };
    bool m_acceptsDMABuf { false };
    Deque<PendingFrame> m_pendingFrames;
};

class GStreamerH264EncoderFactory final : public webrtc::VideoEncoderFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(GStreamerH264EncoderFactory);

public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final { return h264EncoderFormats(); }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&, const webrtc::SdpVideoFormat& format) final
    {
        return std::make_unique<LibWebRTCGStreamerVideoEncoder>(format);
    }
};

class LibWebRTCInternalVideoEncoderFactory final : public webrtc::VideoEncoderFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(LibWebRTCInternalVideoEncoderFactory);

public:
    LibWebRTCInternalVideoEncoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2, bool isSupportingAV1)
        : m_isSupportingVP9Profile0(isSupportingVP9Profile0)
        , m_isSupportingVP9Profile2(isSupportingVP9Profile2)
        , m_isSupportingAV1(isSupportingAV1)
        , m_isSupportingH264(GStreamerRegistryScanner::singleton().isCodecSupported(GStreamerRegistryScanner::Configuration::Encoding, "avc1"_s))
    {
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final
    {
        std::vector<webrtc::SdpVideoFormat> formats = webrtc::LibvpxVp8EncoderTemplateAdapter::SupportedFormats();
        if (m_isSupportingH264) {
            for (auto& format : m_h264Factory.GetSupportedFormats())
                formats.push_back(WTF::move(format));
        }
#if USE(LIBWEBRTC_AV1)
        if (m_isSupportingAV1) {
            for (auto& format : webrtc::LibaomAv1EncoderTemplateAdapter::SupportedFormats())
                formats.push_back(WTF::move(format));
        }
#endif
        for (auto& format : webrtc::SupportedVP9Codecs(true)) {
            if ((m_isSupportingVP9Profile0 && format.IsSameCodec(webrtc::SdpVideoFormat::VP9Profile0())) || (m_isSupportingVP9Profile2 && format.IsSameCodec(webrtc::SdpVideoFormat::VP9Profile2())))
                formats.push_back(WTF::move(format));
        }
        return formats;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format) final
    {
        if (format.IsSameCodec(webrtc::SdpVideoFormat::VP8()))
            return webrtc::CreateVp8Encoder(environment);
        if (format.IsSameCodec(webrtc::SdpVideoFormat::VP9Profile0()))
            return webrtc::CreateVp9Encoder(environment, { webrtc::VP9Profile::kProfile0 });
        if (format.IsSameCodec(webrtc::SdpVideoFormat::VP9Profile2()))
            return webrtc::CreateVp9Encoder(environment, { webrtc::VP9Profile::kProfile2 });
#if USE(LIBWEBRTC_AV1)
        if (m_isSupportingAV1 && format.IsSameCodec(webrtc::SdpVideoFormat::AV1Profile0()))
            return webrtc::CreateLibaomAv1Encoder(environment);
#endif
        if (m_isSupportingH264 && equalLettersIgnoringASCIICase(fromStdString(format.name), "h264"_s))
            return m_h264Factory.Create(environment, format);
        return nullptr;
    }

private:
    bool m_isSupportingVP9Profile0;
    bool m_isSupportingVP9Profile2;
    bool m_isSupportingAV1;
    bool m_isSupportingH264;
    GStreamerH264EncoderFactory m_h264Factory;
};

std::unique_ptr<webrtc::VideoEncoder> GStreamerVideoEncoderFactory::Create(const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format)
{
    if (!format.IsCodecInList(m_internalFactory->GetSupportedFormats())) {
        GST_WARNING("No encoder for %s", format.ToString().c_str());
        return nullptr;
    }
    return std::make_unique<webrtc::SimulcastEncoderAdapter>(environment, m_internalFactory.get(), nullptr, format);
}

GStreamerVideoEncoderFactory::GStreamerVideoEncoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2, bool isSupportingAV1)
{
    ensureGStreamerInitialized();
    registerWebKitGStreamerElements();

    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_webrtcenc_debug, "webkitlibwebrtcvideoencoder", 0, "WebKit WebRTC video encoder");
    });
    m_internalFactory = std::make_unique<LibWebRTCInternalVideoEncoderFactory>(isSupportingVP9Profile0, isSupportingVP9Profile2, isSupportingAV1);
}

GStreamerVideoEncoderFactory::~GStreamerVideoEncoderFactory() = default;

std::vector<webrtc::SdpVideoFormat> GStreamerVideoEncoderFactory::GetSupportedFormats() const
{
    return m_internalFactory->GetSupportedFormats();
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // USE(LIBWEBRTC) && USE(GSTREAMER)
