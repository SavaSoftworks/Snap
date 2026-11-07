/*
 * Copyright (C) 2018 Metrological Group B.V.
 * Copyright (C) 2020 Igalia S.L.
 * Author: Thibault Saunier <tsaunier@igalia.com>
 * Author: Alejandro G. Castro <alex@igalia.com>
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

#pragma once

#if ENABLE(MEDIA_STREAM) && USE(GSTREAMER)

#include "GStreamerCapturer.h"
#include <wtf/text/CString.h>

namespace WTF {
class MediaTime;
}

namespace WebCore {

class GStreamerAudioCapturer final : public GStreamerCapturer {
public:
    GStreamerAudioCapturer(GStreamerCaptureDevice&&);
    GStreamerAudioCapturer(const PipeWireCaptureDevice&);
    ~GStreamerAudioCapturer();

    GstElement* createConverter() final;
    ASCIILiteral name() final { return "Audio"_s; }

    bool setSampleRate(int);
    void setVoiceProcessing(bool);

    using SinkAudioDataCallback = Function<void(GRefPtr<GstSample>&&, WTF::MediaTime&&)>;
    void setSinkAudioCallback(SinkAudioDataCallback&&);

private:
    void handleSample(GRefPtr<GstSample>&&);
    void addVoiceProcessor(GstBin*, bool hasNoiseSuppressor);
    bool m_voiceProcessing { false };
    std::optional<CString> m_echoProbeName;
    std::pair<GStreamerCapturer::SinkSignalsHolder, SinkAudioDataCallback> m_sinkAudioDataCallback;
};

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
