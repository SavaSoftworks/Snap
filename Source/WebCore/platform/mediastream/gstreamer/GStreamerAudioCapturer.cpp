/*
 * Copyright (C) 2018 Metrological Group B.V.
 * Copyright (C) 2020 Igalia S.L.
 * Author: Thibault Saunier <tsaunier@igalia.com>
 * Author: Alejandro G. Castro  <alex@igalia.com>
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

#if ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
#include "GStreamerAudioCapturer.h"

#include "GStreamerAudioMixer.h"
#include "GStreamerCommon.h"

#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <wtf/MediaTime.h>

#if USE(LIBWEBRTC)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <webrtc/api/audio/audio_processing.h>
#include <webrtc/api/audio/builtin_audio_processing_builder.h>
#include <webrtc/api/environment/environment_factory.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

namespace WebCore {

GST_DEBUG_CATEGORY(webkit_audio_capturer_debug);
#define GST_CAT_DEFAULT webkit_audio_capturer_debug

static void initializeAudioCapturerDebugCategory()
{
    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_audio_capturer_debug, "webkitaudiocapturer", 0, "WebKit Audio Capturer");
    });
}

GStreamerAudioCapturer::GStreamerAudioCapturer(GStreamerCaptureDevice&& device)
    : GStreamerCapturer(WTF::move(device), adoptGRef(gst_caps_new_empty_simple("audio/x-raw")))
{
    initializeAudioCapturerDebugCategory();
}

GStreamerAudioCapturer::GStreamerAudioCapturer(const PipeWireCaptureDevice& device)
    : GStreamerCapturer(device)
{
    initializeAudioCapturerDebugCategory();
}

void GStreamerAudioCapturer::handleSample(GRefPtr<GstSample>&& sample)
{
    auto presentationTime = fromGstClockTime(GST_BUFFER_PTS(gst_sample_get_buffer(sample.get())));
    m_sinkAudioDataCallback.second(WTF::move(sample), WTF::move(presentationTime));
}

void GStreamerAudioCapturer::setSinkAudioCallback(SinkAudioDataCallback&& callback)
{
    if (m_sinkAudioDataCallback.first.newSampleSignalId)
        g_signal_handler_disconnect(sink(), m_sinkAudioDataCallback.first.newSampleSignalId);

    m_sinkAudioDataCallback.second = WTF::move(callback);
    m_sinkAudioDataCallback.first.newSampleSignalId = g_signal_connect_swapped(sink(), "new-sample", G_CALLBACK(+[](GStreamerAudioCapturer* capturer, GstElement* sink) -> GstFlowReturn {
        GRefPtr sample = adoptGRef(gst_app_sink_pull_sample(GST_APP_SINK(sink)));
        capturer->handleSample(WTF::move(sample));
        return GST_FLOW_OK;
    }), this);
}

#if USE(LIBWEBRTC)
class CaptureGainController {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CaptureGainController);

public:
    CaptureGainController()
    {
        webrtc::AudioProcessing::Config config;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = true;
        config.gain_controller2.adaptive_digital.enabled = true;
        config.gain_controller2.input_volume_controller.enabled = false;
        config.gain_controller2.fixed_digital.gain_db = 0;
        m_processor = webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
        if (!m_processor)
            GST_WARNING("Could not create the capture gain controller");
    }

    void setFormat(const GstCaps* caps)
    {
        GstAudioInfo info;
        if (!caps || !gst_audio_info_from_caps(&info, caps) || GST_AUDIO_INFO_FORMAT(&info) != GST_AUDIO_FORMAT_S16 || GST_AUDIO_INFO_LAYOUT(&info) != GST_AUDIO_LAYOUT_INTERLEAVED || GST_AUDIO_INFO_RATE(&info) % 100) {
            m_sampleRate = 0;
            m_channels = 0;
            GST_WARNING("The capture gain controller cannot process %" GST_PTR_FORMAT, caps);
            return;
        }
        m_sampleRate = GST_AUDIO_INFO_RATE(&info);
        m_channels = GST_AUDIO_INFO_CHANNELS(&info);
    }

    void process(GstBuffer* buffer)
    {
        if (!m_processor || !m_sampleRate || !m_channels)
            return;

        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE))
            return;

        size_t frameSamples = static_cast<size_t>(m_sampleRate / 100) * m_channels;
        auto samples = unsafeMakeSpan(reinterpret_cast<int16_t*>(map.data), map.size / sizeof(int16_t));
        webrtc::StreamConfig streamConfig(m_sampleRate, m_channels);
        size_t offset = 0;
        for (; offset + frameSamples <= samples.size(); offset += frameSamples) {
            auto frame = samples.subspan(offset, frameSamples);
            m_processor->ProcessStream(frame.data(), streamConfig, streamConfig, frame.data());
        }
        if (offset != samples.size())
            GST_WARNING("The capture gain controller skipped %zu samples outside 10 ms frames", samples.size() - offset);
        gst_buffer_unmap(buffer, &map);
    }

private:
    webrtc::scoped_refptr<webrtc::AudioProcessing> m_processor;
    int m_sampleRate { 0 };
    size_t m_channels { 0 };
};

static GstPadProbeReturn applyCaptureGain(GstPad*, GstPadProbeInfo* info, gpointer userData)
{
    auto& controller = *static_cast<CaptureGainController*>(userData);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        auto* event = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
            GstCaps* caps = nullptr;
            gst_event_parse_caps(event, &caps);
            controller.setFormat(caps);
        }
        return GST_PAD_PROBE_OK;
    }

    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;
    buffer = gst_buffer_make_writable(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    controller.process(buffer);
    return GST_PAD_PROBE_OK;
}
#endif

GStreamerAudioCapturer::~GStreamerAudioCapturer()
{
    if (m_echoProbeName)
        GStreamerAudioMixer::singleton().releaseEchoReference();
}

void GStreamerAudioCapturer::setVoiceProcessing(bool enabled)
{
    m_voiceProcessing = enabled;
    if (enabled || !m_echoProbeName)
        return;

    GStreamerAudioMixer::singleton().releaseEchoReference();
    m_echoProbeName = std::nullopt;
}

void GStreamerAudioCapturer::addVoiceProcessor(GstBin* bin, bool hasNoiseSuppressor)
{
    static constexpr int voiceProcessingSampleRate = 48000;
    if (!m_voiceProcessing)
        return;

    if (!m_echoProbeName)
        m_echoProbeName = GStreamerAudioMixer::singleton().acquireEchoReference();
    if (!m_echoProbeName) {
        GST_WARNING("Echo cancellation was requested but no echo reference is available");
        return;
    }

    GRefPtr chainInput = adoptGRef(gst_bin_find_unlinked_pad(bin, GST_PAD_SINK));
    if (!chainInput)
        return;

    auto* processor = makeGStreamerElement("webrtcdsp"_s);
    if (!processor)
        return;

    auto* processorConvert = makeGStreamerElement("audioconvert"_s);
    auto* processorResample = makeGStreamerElement("audioresample"_s);
    auto* processorFormat = gst_element_factory_make("capsfilter", nullptr);
    GRefPtr caps = adoptGRef(gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, voiceProcessingSampleRate, nullptr));
    g_object_set(processorFormat, "caps", caps.get(), nullptr);
#if USE(LIBWEBRTC)
    bool usesDigitalGainControl = false;
#else
    bool usesDigitalGainControl = true;
#endif
    g_object_set(processor, "probe", m_echoProbeName->data(), "echo-cancel", TRUE, "gain-control", usesDigitalGainControl, "noise-suppression", !hasNoiseSuppressor, "high-pass-filter", TRUE, "delay-agnostic", TRUE, "extended-filter", TRUE, "voice-detection", FALSE, nullptr);
    gst_util_set_object_arg(G_OBJECT(processor), "noise-suppression-level", "high");

    gst_bin_add_many(bin, processorConvert, processorResample, processorFormat, processor, nullptr);
    gst_element_link_many(processorConvert, processorResample, processorFormat, processor, nullptr);
    GRefPtr processorOutput = adoptGRef(gst_element_get_static_pad(processor, "src"));
#if USE(LIBWEBRTC)
    gst_pad_add_probe(processorOutput.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), applyCaptureGain, new CaptureGainController, [](gpointer data) {
        delete static_cast<CaptureGainController*>(data);
    });
#endif
    gst_pad_link(processorOutput.get(), chainInput.get());
    GST_DEBUG("Voice processing enabled with echo reference %s", m_echoProbeName->data());
}

GstElement* GStreamerAudioCapturer::createConverter()
{
    auto* bin = gst_bin_new(nullptr);
    auto* audioconvert = makeGStreamerElement("audioconvert"_s);
    auto* audioresample = makeGStreamerElement("audioresample"_s);
    gst_bin_add_many(GST_BIN_CAST(bin), audioconvert, audioresample, nullptr);
    gst_element_link(audioconvert, audioresample);

    bool hasNoiseSuppressor = false;
#if USE(GSTREAMER_WEBRTC)
    if (auto audioFilter = makeGStreamerElement("audiornnoise"_s)) {
        hasNoiseSuppressor = true;
        auto audioconvert2 = makeGStreamerElement("audioconvert"_s);
        auto audioresample2 = makeGStreamerElement("audioresample"_s);
        gst_bin_add_many(GST_BIN_CAST(bin), audioconvert2, audioresample2, audioFilter, nullptr);
        gst_element_link_many(audioconvert2, audioresample2, audioFilter, audioconvert, nullptr);
    }
#endif

    addVoiceProcessor(GST_BIN_CAST(bin), hasNoiseSuppressor);

    if (GRefPtr pad = adoptGRef(gst_bin_find_unlinked_pad(GST_BIN_CAST(bin), GST_PAD_SRC)))
        gst_element_add_pad(GST_ELEMENT_CAST(bin), gst_ghost_pad_new("src", pad.get()));
    if (GRefPtr pad = adoptGRef(gst_bin_find_unlinked_pad(GST_BIN_CAST(bin), GST_PAD_SINK)))
        gst_element_add_pad(GST_ELEMENT_CAST(bin), gst_ghost_pad_new("sink", pad.get()));

    return bin;
}

bool GStreamerAudioCapturer::setSampleRate(int sampleRate)
{
    if (sampleRate <= 0) {
        GST_INFO_OBJECT(m_pipeline.get(), "Not forcing sample rate");
        return false;
    }

    GST_INFO_OBJECT(m_pipeline.get(), "Setting SampleRate %d", sampleRate);
    m_caps = adoptGRef(gst_caps_new_simple("audio/x-raw", "rate", G_TYPE_INT, sampleRate, nullptr));

    if (!m_capsfilter)
        return false;

    g_object_set(m_capsfilter.get(), "caps", m_caps.get(), nullptr);
    return true;
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
