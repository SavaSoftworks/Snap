/*
 *  Copyright (C) 2019-2022 Igalia S.L. All rights reserved.
 *  Copyright (C) 2022 Metrological Group B.V.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "GStreamerRtpTransceiverBackend.h"

#if ENABLE(WEB_RTC) && USE(GSTREAMER_WEBRTC)

#include "ExceptionOr.h"
#include "GStreamerRtpReceiverBackend.h"
#include "GStreamerRtpSenderBackend.h"
#include "GStreamerWebRTCUtils.h"
#include "RTCRtpCodec.h"
#include <array>
#include <gst/pbutils/codec-utils.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/glib/GMallocString.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/StringToIntegerConversion.h>

GST_DEBUG_CATEGORY(webkit_webrtc_transceiver_debug);
#define GST_CAT_DEFAULT webkit_webrtc_transceiver_debug

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerRtpTransceiverBackend);

GStreamerRtpTransceiverBackend::GStreamerRtpTransceiverBackend(GRefPtr<GstWebRTCRTPTransceiver>&& rtcTransceiver, std::optional<uint32_t> sourceSSRC)
    : m_rtcTransceiver(WTF::move(rtcTransceiver))
    , m_sourceSSRC(sourceSSRC)
{
    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_webrtc_transceiver_debug, "webkitwebrtcrtptransceiver", 0, "WebKit WebRTC RTP transceiver");
    });

    if (m_sourceSSRC) {
        GRefPtr<GstCaps> caps;
        g_object_get(m_rtcTransceiver.get(), "codec-preferences", &caps.outPtr(), nullptr);
        if (caps) {
            caps = adoptGRef(gst_caps_make_writable(caps.leakRef()));
            applySourceSSRC(caps.get(), direction());
            g_object_set(m_rtcTransceiver.get(), "codec-preferences", caps.get(), nullptr);
        }
    }

    GstWebRTCKind kind;
    g_object_get(m_rtcTransceiver.get(), "kind", &kind, nullptr);

    // FIXME: The ulp/red encoders drop MID extension headers. See also:
    // https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/923
    // gst_util_set_object_arg(G_OBJECT(m_rtcTransceiver.get()), "fec-type", "ulp-red");

    // Enable nack only for video transceivers, so that RTX payloads are not signaled in SDP
    // offer/answer. Those are confusing some media servers... Internally webrtcbin will always
    // setup RTX, RED and FEC anyway.
    if (kind != GST_WEBRTC_KIND_VIDEO)
        return;

    g_object_set(m_rtcTransceiver.get(), "do-nack", TRUE, nullptr);
}

UniqueRef<GStreamerRtpReceiverBackend> GStreamerRtpTransceiverBackend::createReceiverBackend()
{
    return WTF::makeUniqueRef<GStreamerRtpReceiverBackend>(GRefPtr(m_rtcTransceiver));
}

Ref<GStreamerRtpSenderBackend> GStreamerRtpTransceiverBackend::createSenderBackend(WeakPtr<GStreamerPeerConnectionBackend>&& backend, GStreamerRtpSenderBackend::Source&& source, GUniquePtr<GstStructure>&& initData)
{
    GRefPtr<GstWebRTCRTPSender> sender;
    g_object_get(m_rtcTransceiver.get(), "sender", &sender.outPtr(), nullptr);
    return GStreamerRtpSenderBackend::create(WTF::move(backend), WTF::move(sender), WTF::move(source), WTF::move(initData));
}

RTCRtpTransceiverDirection GStreamerRtpTransceiverBackend::direction() const
{
    GstWebRTCRTPTransceiverDirection gstDirection;
    g_object_get(m_rtcTransceiver.get(), "direction", &gstDirection, nullptr);
    return toRTCRtpTransceiverDirection(gstDirection);
}

std::optional<RTCRtpTransceiverDirection> GStreamerRtpTransceiverBackend::currentDirection() const
{
    GstWebRTCRTPTransceiverDirection gstDirection;
    g_object_get(m_rtcTransceiver.get(), "current-direction", &gstDirection, nullptr);
    if (!gstDirection)
        return { };

    return toRTCRtpTransceiverDirection(gstDirection);
}

void GStreamerRtpTransceiverBackend::applySourceSSRC(GstCaps* caps, RTCRtpTransceiverDirection direction)
{
    if (!m_sourceSSRC || gst_caps_is_empty(caps) || gst_caps_is_any(caps))
        return;
    bool sends = direction == RTCRtpTransceiverDirection::Sendonly || direction == RTCRtpTransceiverDirection::Sendrecv;
    m_hasSendingSSRC |= sends;
    for (unsigned i = 0; i < gst_caps_get_size(caps); ++i) {
        auto* structure = gst_caps_get_structure(caps, i);
        if (m_hasSendingSSRC && !i)
            gst_structure_set(structure, "ssrc", G_TYPE_UINT, *m_sourceSSRC, nullptr);
        else
            gst_structure_remove_field(structure, "ssrc");
    }
}

void GStreamerRtpTransceiverBackend::setDirection(RTCRtpTransceiverDirection direction)
{
    auto gstDirection = fromRTCRtpTransceiverDirection(direction);
#ifndef GST_DISABLE_GST_DEBUG
    auto directionString = GMallocString::unsafeAdoptFromUTF8(g_enum_to_string(GST_TYPE_WEBRTC_RTP_TRANSCEIVER_DIRECTION, gstDirection));
    GST_DEBUG_OBJECT(m_rtcTransceiver.get(), "Setting direction to %s", directionString.utf8());
#endif
    GRefPtr<GstCaps> caps;
    if (m_sourceSSRC)
        g_object_get(m_rtcTransceiver.get(), "codec-preferences", &caps.outPtr(), nullptr);
    if (caps) {
        caps = adoptGRef(gst_caps_make_writable(caps.leakRef()));
        applySourceSSRC(caps.get(), direction);
        g_object_set(m_rtcTransceiver.get(), "direction", gstDirection, "codec-preferences", caps.get(), nullptr);
    } else
        g_object_set(m_rtcTransceiver.get(), "direction", gstDirection, nullptr);
}

String GStreamerRtpTransceiverBackend::mid()
{
    GUniqueOutPtr<char> mid;
    g_object_get(m_rtcTransceiver.get(), "mid", &mid.outPtr(), nullptr);
    return String::fromUTF8(mid.get());
}

void GStreamerRtpTransceiverBackend::stop()
{
    // Ideally we should also stop webrtcbin transceivers but it's not supported yet.
    m_isStopped = true;
}

bool GStreamerRtpTransceiverBackend::stopped() const
{
    // Ideally this should be queried on webrtcbin, but its transceivers can't be stopped yet.
    return m_isStopped;
}

static bool isDerivedRedundancyCodec(const String& codecName)
{
    return equalLettersIgnoringASCIICase(codecName, "rtx"_s) || equalLettersIgnoringASCIICase(codecName, "red"_s) || equalLettersIgnoringASCIICase(codecName, "ulpfec"_s) || equalLettersIgnoringASCIICase(codecName, "flexfec-03"_s);
}

static Vector<std::pair<String, String>> codecFormatParameters(const String& line)
{
    Vector<std::pair<String, String>> parameters;
    for (auto& attribute : line.split(';')) {
        auto separator = attribute.find('=');
        if (separator == notFound)
            continue;
        auto name = attribute.left(separator).trim([](char16_t character) { return isASCIIWhitespace(character); });
        auto value = attribute.substring(separator + 1).trim([](char16_t character) { return isASCIIWhitespace(character); });
        if (!name.isEmpty())
            parameters.append({ WTF::move(name), WTF::move(value) });
    }
    return parameters;
}

static unsigned codecChannelCount(const GstStructure* structure)
{
    int channels = 0;
    if (gst_structure_get_int(structure, "channels", &channels) && channels > 0)
        return channels;
    if (const char* parameters = gst_structure_get_string(structure, "encoding-params")) {
        if (auto count = parseInteger<unsigned>(StringView::fromLatin1(parameters)); count && *count)
            return *count;
    }
    return 1;
}

static String h264ProfileName(const String& profileLevelId)
{
    if (profileLevelId.length() != 6)
        return {};
    std::array<uint8_t, 3> bytes {};
    for (unsigned index = 0; index < bytes.size(); index++) {
        auto value = parseInteger<uint8_t>(StringView(profileLevelId).substring(index * 2, 2), 16);
        if (!value)
            return {};
        bytes[index] = *value;
    }
    if (const char* profile = gst_codec_utils_h264_get_profile(bytes.data(), bytes.size()))
        return String::fromLatin1(profile);
    return {};
}

static String codecParameter(const Vector<std::pair<String, String>>& parameters, ASCIILiteral name)
{
    for (auto& [key, value] : parameters) {
        if (equalIgnoringASCIICase(key, name))
            return value;
    }
    return {};
}

static String structureParameter(const GstStructure* structure, ASCIILiteral name)
{
    if (const char* value = gst_structure_get_string(structure, name.characters()))
        return String::fromLatin1(value);
    return {};
}

static bool sameCodecParameter(String requested, String current, ASCIILiteral fallback)
{
    if (requested.isNull())
        requested = fallback;
    if (current.isNull())
        current = fallback;
    return equalIgnoringASCIICase(requested, current);
}

static bool codecMatchesStructure(const RTCRtpCodec& codec, const String& mediaType, const String& codecName, const Vector<std::pair<String, String>>& parameters, const GstStructure* structure, bool compareAllParameters)
{
    if (const char* media = gst_structure_get_string(structure, "media"); media && !equalIgnoringASCIICase(StringView::fromLatin1(media), mediaType))
        return false;
    const char* encodingName = gst_structure_get_string(structure, "encoding-name");
    if (!encodingName || !equalIgnoringASCIICase(StringView::fromLatin1(encodingName), codecName))
        return false;
    int clockRate = 0;
    if (!gst_structure_get_int(structure, "clock-rate", &clockRate) || clockRate != static_cast<int>(codec.clockRate))
        return false;
    if (codec.channels && codecChannelCount(structure) != *codec.channels)
        return false;

    if (equalLettersIgnoringASCIICase(codecName, "h264"_s)) {
        if (!sameCodecParameter(codecParameter(parameters, "packetization-mode"_s), structureParameter(structure, "packetization-mode"_s), "0"_s))
            return false;
        auto requestedProfile = h264ProfileName(codecParameter(parameters, "profile-level-id"_s));
        auto currentProfile = structureParameter(structure, "profile"_s);
        if (currentProfile.isNull())
            currentProfile = h264ProfileName(structureParameter(structure, "profile-level-id"_s));
        if (!requestedProfile.isNull() && !currentProfile.isNull() && !equalIgnoringASCIICase(requestedProfile, currentProfile))
            return false;
    } else if (equalLettersIgnoringASCIICase(codecName, "vp9"_s)) {
        if (!sameCodecParameter(codecParameter(parameters, "profile-id"_s), structureParameter(structure, "profile-id"_s), "0"_s))
            return false;
    } else if (equalLettersIgnoringASCIICase(codecName, "av1"_s)) {
        if (!sameCodecParameter(codecParameter(parameters, "profile"_s), structureParameter(structure, "profile"_s), "0"_s))
            return false;
    }

    if (!compareAllParameters)
        return true;
    for (auto& [key, value] : parameters) {
        if (equalLettersIgnoringASCIICase(key, "profile-level-id"_s) || equalLettersIgnoringASCIICase(key, "profile"_s))
            continue;
        auto field = key.utf8();
        if (const char* current = gst_structure_get_string(structure, field.data()); current && !equalIgnoringASCIICase(StringView::fromLatin1(current), value))
            return false;
    }
    return true;
}

ExceptionOr<void> GStreamerRtpTransceiverBackend::setCodecPreferences(const Vector<RTCRtpCodec>& codecs)
{
    GRefPtr<GstCaps> currentCaps;
    g_object_get(m_rtcTransceiver.get(), "codec-preferences", &currentCaps.outPtr(), nullptr);
    GST_TRACE_OBJECT(m_rtcTransceiver.get(), "Current codec preferences: %" GST_PTR_FORMAT, currentCaps.get());
    if (codecs.isEmpty() || !currentCaps || gst_caps_is_empty(currentCaps.get()))
        return {};

    Vector<const GstStructure*> negotiatedCodecs;
    for (unsigned index = 0; index < gst_caps_get_size(currentCaps.get()); index++)
        negotiatedCodecs.append(gst_caps_get_structure(currentCaps.get(), index));
    const GstStructure* extensionSource = negotiatedCodecs[0];
    String msid;
    for (auto* structure : negotiatedCodecs) {
        if (const char* value = gst_structure_get_string(structure, "a-msid")) {
            msid = String::fromUTF8(value);
            break;
        }
    }

    Vector<bool> consumed;
    consumed.fill(false, negotiatedCodecs.size());
    GRefPtr gstCodecs = adoptGRef(gst_caps_new_empty());
    for (auto& codec : codecs) {
        if (!codec.mimeType.startsWithIgnoringASCIICase("video/"_s) && !codec.mimeType.startsWithIgnoringASCIICase("audio/"_s))
            return Exception { ExceptionCode::InvalidModificationError, "RTCRtpCodec bad mimeType"_s };

        auto separator = codec.mimeType.find('/');
        auto mediaType = codec.mimeType.left(separator).convertToASCIILowercase();
        auto codecName = codec.mimeType.substring(separator + 1);
        if (codecName.isEmpty())
            return Exception { ExceptionCode::InvalidModificationError, "RTCRtpCodec bad mimeType"_s };
        if (isDerivedRedundancyCodec(codecName))
            continue;

        auto parameters = codecFormatParameters(codec.sdpFmtpLine);
        std::optional<size_t> match;
        for (bool compareAllParameters : { true, false }) {
            for (size_t index = 0; index < negotiatedCodecs.size() && !match; index++) {
                if (!consumed[index] && codecMatchesStructure(codec, mediaType, codecName, parameters, negotiatedCodecs[index], compareAllParameters))
                    match = index;
            }
            if (match)
                break;
        }
        if (!match) {
            GST_DEBUG_OBJECT(m_rtcTransceiver.get(), "No negotiated payload type for %s, leaving it out of the codec preferences", codec.mimeType.utf8().data());
            continue;
        }

        consumed[*match] = true;
        auto* structure = gst_structure_copy(negotiatedCodecs[*match]);
        for (int field = 0; field < gst_structure_n_fields(extensionSource); field++) {
            const char* name = gst_structure_nth_field_name(extensionSource, field);
            if (g_str_has_prefix(name, "extmap-") && !gst_structure_has_field(structure, name))
                gst_structure_set_value(structure, name, gst_structure_get_value(extensionSource, name));
        }
        gst_caps_append_structure(gstCodecs.get(), structure);
    }

    if (gst_caps_is_empty(gstCodecs.get())) {
        GST_DEBUG_OBJECT(m_rtcTransceiver.get(), "None of the preferred codecs is negotiable, keeping the current codec preferences");
        return {};
    }

    if (!msid.isEmpty()) {
        auto* first = gst_caps_get_structure(gstCodecs.get(), 0);
        if (!gst_structure_has_field(first, "a-msid"))
            gst_structure_set(first, "a-msid", G_TYPE_STRING, msid.utf8().data(), nullptr);
    }
    GST_DEBUG_OBJECT(m_rtcTransceiver.get(), "Setting codec preferences to %" GST_PTR_FORMAT, gstCodecs.get());
    applySourceSSRC(gstCodecs.get(), direction());
    g_object_set(m_rtcTransceiver.get(), "codec-preferences", gstCodecs.get(), nullptr);
    return { };
}

void GStreamerRtpTransceiverBackend::tearDown()
{
    m_rtcTransceiver.clear();
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // ENABLE(WEB_RTC) && USE(GSTREAMER_WEBRTC)
