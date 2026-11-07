/*
 * Copyright (C) 2017 Igalia S.L
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
#include "RealtimeOutgoingAudioSourceLibWebRTC.h"

#include "GStreamerAudioData.h"
#include "GStreamerAudioStreamDescription.h"
#include "LibWebRTCAudioFormat.h"
#include "LibWebRTCProvider.h"
#include "NotImplemented.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

static constexpr size_t maximumPendingChunks = 50;

WTF_MAKE_TZONE_ALLOCATED_IMPL(RealtimeOutgoingAudioSourceLibWebRTC);

GST_DEBUG_CATEGORY(webkit_libwebrtc_outgoing_audio_debug);
#define GST_CAT_DEFAULT webkit_libwebrtc_outgoing_audio_debug

RealtimeOutgoingAudioSourceLibWebRTC::RealtimeOutgoingAudioSourceLibWebRTC(Ref<MediaStreamTrackPrivate>&& audioSource)
    : RealtimeOutgoingAudioSource(WTF::move(audioSource))
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_libwebrtc_outgoing_audio_debug, "webkitlibwebrtcaudiooutgoing", 0, "WebKit LibWebRTC outgoing audio source");
    });
    Locker locker { m_conversionLock };
    gst_audio_info_init(&m_inputStreamDescription);
    gst_audio_info_init(&m_outputStreamDescription);
}

RealtimeOutgoingAudioSourceLibWebRTC::~RealtimeOutgoingAudioSourceLibWebRTC()
{
    Locker locker { m_conversionLock };
    m_sampleConverter = nullptr;
}

Ref<RealtimeOutgoingAudioSource> RealtimeOutgoingAudioSource::create(Ref<MediaStreamTrackPrivate>&& audioSource)
{
    return RealtimeOutgoingAudioSourceLibWebRTC::create(WTF::move(audioSource));
}

static inline GstAudioInfo libwebrtcAudioFormat(int sampleRate, size_t channelCount)
{
    GstAudioFormat format = gst_audio_format_build_integer(
        LibWebRTCAudioFormat::isSigned,
        LibWebRTCAudioFormat::isBigEndian ? G_BIG_ENDIAN : G_LITTLE_ENDIAN,
        LibWebRTCAudioFormat::sampleSize,
        LibWebRTCAudioFormat::sampleSize);

    GstAudioInfo info;

    size_t libWebRTCChannelCount = channelCount >= 2 ? 2 : channelCount;
    gst_audio_info_set_format(&info, format, sampleRate, libWebRTCChannelCount, nullptr);
    return info;
}

void RealtimeOutgoingAudioSourceLibWebRTC::audioSamplesAvailable(const MediaTime&, const PlatformAudioData& audioData, const AudioStreamDescription& streamDescription, size_t /* sampleCount */)
{
    DisableMallocRestrictionsForCurrentThreadScope disableMallocRestrictions;
    const auto& data = static_cast<const GStreamerAudioData&>(audioData);
    const auto& desc = static_cast<const GStreamerAudioStreamDescription&>(streamDescription);
    auto* buffer = gst_sample_get_buffer(data.getSample().get());
    if (!buffer)
        return;

    {
        Locker locker { m_conversionLock };
        if (!m_sampleConverter || !gst_audio_info_is_equal(&m_inputStreamDescription, &desc.getInfo())) {
            if (m_sampleConverter)
                GST_DEBUG("Input audio format changed, dropping %zu converted bytes", m_convertedAudio.size());
            m_convertedAudio.clear();
            m_sampleConverter = nullptr;
            m_inputStreamDescription = desc.getInfo();
            m_outputStreamDescription = libwebrtcAudioFormat(LibWebRTCAudioFormat::sampleRate, desc.numberOfChannels());
#ifndef GST_DISABLE_GST_DEBUG
            GRefPtr inputCaps = adoptGRef(gst_audio_info_to_caps(&m_inputStreamDescription));
            GRefPtr outputCaps = adoptGRef(gst_audio_info_to_caps(&m_outputStreamDescription));
            GST_DEBUG("Converting from %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, inputCaps.get(), outputCaps.get());
#endif
            m_sampleConverter.reset(gst_audio_converter_new(GST_AUDIO_CONVERTER_FLAG_NONE, &m_inputStreamDescription, &m_outputStreamDescription, nullptr));
            if (!m_sampleConverter) {
                GST_ERROR("Unable to convert the captured audio format for LibWebRTC");
                return;
            }
        }

        GstAudioBuffer captured;
        if (!gst_audio_buffer_map(&captured, &m_inputStreamDescription, buffer, GST_MAP_READ)) {
            GST_WARNING("Could not map %zu bytes of captured audio", gst_buffer_get_size(buffer));
            return;
        }
        size_t inFrames = GST_AUDIO_BUFFER_N_SAMPLES(&captured);
        size_t outFrameSize = GST_AUDIO_INFO_BPF(&m_outputStreamDescription);
        size_t outFrames = gst_audio_converter_get_out_frames(m_sampleConverter.get(), inFrames);
        size_t offset = m_convertedAudio.size();
        m_convertedAudio.grow(offset + outFrames * outFrameSize);
        auto output = m_convertedAudio.mutableSpan().subspan(offset);
        gpointer out[1] = { output.data() };
        bool converted = gst_audio_converter_samples(m_sampleConverter.get(), GST_AUDIO_CONVERTER_FLAG_NONE, captured.planes, inFrames, out, outFrames);
        gst_audio_buffer_unmap(&captured);
        if (!converted)
            GST_WARNING("Could not convert %zu captured audio frames", inFrames);
        if ((!converted || isSilenced()) && !output.empty())
            webkitGstAudioFormatFillSilence(m_outputStreamDescription.finfo, output.data(), output.size());

        size_t maximumPendingSize = maximumPendingChunks * LibWebRTCAudioFormat::chunkSampleCount * outFrameSize;
        if (m_convertedAudio.size() > maximumPendingSize) {
            size_t excess = m_convertedAudio.size() - maximumPendingSize;
            GST_WARNING("Dropping %zu bytes of captured audio that LibWebRTC has not taken", excess);
            m_convertedAudio.removeAt(0, excess);
        }
    }
    LibWebRTCProvider::callOnWebRTCSignalingThread([protectedThis = protect(*this)] {
        protectedThis->pullAudioData();
    });
}

void RealtimeOutgoingAudioSourceLibWebRTC::pullAudioData()
{
    const size_t outChunkSampleCount = LibWebRTCAudioFormat::chunkSampleCount;
    while (true) {
        int sampleRate = 0;
        size_t channelCount = 0;
        {
            Locker locker { m_conversionLock };
            if (!m_sampleConverter)
                return;

            size_t outChunkSize = outChunkSampleCount * GST_AUDIO_INFO_BPF(&m_outputStreamDescription);
            if (!outChunkSize || m_convertedAudio.size() < outChunkSize)
                return;

            m_audioBuffer.resize(outChunkSize);
            memcpySpan(m_audioBuffer.mutableSpan(), m_convertedAudio.span().first(outChunkSize));
            m_convertedAudio.removeAt(0, outChunkSize);
            sampleRate = GST_AUDIO_INFO_RATE(&m_outputStreamDescription);
            channelCount = GST_AUDIO_INFO_CHANNELS(&m_outputStreamDescription);
        }

        GST_TRACE("Sending audio frames");
        sendAudioFrames(m_audioBuffer.span(), LibWebRTCAudioFormat::sampleSize, sampleRate, channelCount, outChunkSampleCount);
    }
}

bool RealtimeOutgoingAudioSourceLibWebRTC::isReachingBufferedAudioDataHighLimit()
{
    notImplemented();
    return false;
}

bool RealtimeOutgoingAudioSourceLibWebRTC::isReachingBufferedAudioDataLowLimit()
{
    notImplemented();
    return false;
}

bool RealtimeOutgoingAudioSourceLibWebRTC::hasBufferedEnoughData()
{
    notImplemented();
    return false;
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // USE(LIBWEBRTC) && USE(GSTREAMER)
