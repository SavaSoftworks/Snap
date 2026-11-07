/*
 *  Copyright (C) 2012, 2015, 2016, 2018 Igalia S.L
 *  Copyright (C) 2015, 2016, 2018 Metrological Group B.V.
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

#if USE(GSTREAMER) && USE(LIBWEBRTC)
#include "GStreamerVideoFrameLibWebRTC.h"

#include "GStreamerCommon.h"
#include "GStreamerVideoFrameConverter.h"
#include "LibWebRTCVideoFrameUtilities.h"
#include "VideoFrameGStreamer.h"

#include <algorithm>
#include <cmath>
#include <wtf/MediaTime.h>
#include <wtf/glib/GUniquePtr.h>

namespace WebCore {

GST_DEBUG_CATEGORY(webkit_libwebrtc_video_frame_debug);
#define GST_CAT_DEFAULT webkit_libwebrtc_video_frame_debug

static void ensureDebugCategoryIsRegistered()
{
    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_libwebrtc_video_frame_debug, "webkitlibwebrtcvideoframe", 0, "WebKit LibWebRTC Video Frame");
    });
}

static GStreamerVideoFrameLibWebRTC::MemoryType memoryTypeFromCaps(GstCaps* caps)
{
    auto* features = caps && gst_caps_get_size(caps) ? gst_caps_get_features(caps, 0) : nullptr;
    if (!features || gst_caps_features_is_equal(features, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
        return GStreamerVideoFrameLibWebRTC::MemoryType::System;
    if (gst_caps_features_contains(features, "memory:DMABuf"))
        return GStreamerVideoFrameLibWebRTC::MemoryType::DMABuf;
    if (gst_caps_features_contains(features, "memory:GLMemory"))
        return GStreamerVideoFrameLibWebRTC::MemoryType::GL;
    return GStreamerVideoFrameLibWebRTC::MemoryType::Other;
}

static std::optional<IntSize> sampleSize(GstCaps* caps)
{
    auto* structure = caps && gst_caps_get_size(caps) ? gst_caps_get_structure(caps, 0) : nullptr;
    if (!structure)
        return std::nullopt;
    auto width = gstStructureGet<int>(structure, "width"_s);
    auto height = gstStructureGet<int>(structure, "height"_s);
    if (!width || !height || *width <= 0 || *height <= 0)
        return std::nullopt;
    return IntSize { *width, *height };
}

static bool isRGBSample(GstCaps* caps)
{
    auto info = VideoFrameGStreamer::infoFromCaps(GRefPtr(caps));
    return GST_VIDEO_INFO_FORMAT(&info.info) != GST_VIDEO_FORMAT_UNKNOWN && GST_VIDEO_INFO_IS_RGB(&info.info);
}

static webrtc::scoped_refptr<webrtc::I420BufferInterface> wrapI420Sample(GRefPtr<GstSample>&& sample, IntPoint offset, IntSize size)
{
    auto frame = makeUnique<GstMappedFrame>(sample, GST_MAP_READ);
    if (!*frame || frame->format() != GST_VIDEO_FORMAT_I420 || offset.x() % 2 || offset.y() % 2 || offset.x() + size.width() > frame->width() || offset.y() + size.height() > frame->height())
        return nullptr;

    int strideY = frame->componentStride(GST_VIDEO_COMP_Y);
    int strideU = frame->componentStride(GST_VIDEO_COMP_U);
    int strideV = frame->componentStride(GST_VIDEO_COMP_V);
    auto planeY = frame->componentData(GST_VIDEO_COMP_Y).subspan(static_cast<size_t>(offset.y()) * strideY + offset.x());
    auto planeU = frame->componentData(GST_VIDEO_COMP_U).subspan(static_cast<size_t>(offset.y() / 2) * strideU + offset.x() / 2);
    auto planeV = frame->componentData(GST_VIDEO_COMP_V).subspan(static_cast<size_t>(offset.y() / 2) * strideV + offset.x() / 2);
    return webrtc::WrapI420Buffer(size.width(), size.height(), planeY.data(), strideY, planeU.data(), strideU, planeV.data(), strideV, [frame = frame.release()] {
        delete frame;
    });
}

GRefPtr<GstSample> convertLibWebRTCVideoFrameToGStreamerSample(const webrtc::VideoFrame& frame, bool acceptsDMABuf)
{
    auto frameBuffer = frame.video_frame_buffer();
    if (!frameBuffer) [[unlikely]]
        return nullptr;

    if (frameBuffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
        auto& nativeBuffer = static_cast<GStreamerVideoFrameLibWebRTC&>(*frameBuffer);
        switch (nativeBuffer.memoryType()) {
        case GStreamerVideoFrameLibWebRTC::MemoryType::System:
            if (!nativeBuffer.isCropped() && !nativeBuffer.isScaled())
                return nativeBuffer.sample();
            break;
        case GStreamerVideoFrameLibWebRTC::MemoryType::DMABuf:
            if (acceptsDMABuf && !nativeBuffer.isCropped())
                return nativeBuffer.sample();
            break;
        case GStreamerVideoFrameLibWebRTC::MemoryType::GL:
        case GStreamerVideoFrameLibWebRTC::MemoryType::Other:
            break;
        }
    }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420Buffer = frameBuffer->ToI420();
    if (!i420Buffer) [[unlikely]]
        return nullptr;

    auto isTightlyPacked = [](const webrtc::I420BufferInterface& buffer) {
        auto chromaHeight = static_cast<ptrdiff_t>((buffer.height() + 1) / 2);
        auto offsetU = buffer.DataU() - buffer.DataY();
        auto offsetV = buffer.DataV() - buffer.DataY();
        return offsetU == static_cast<ptrdiff_t>(buffer.StrideY()) * buffer.height() && offsetV == offsetU + static_cast<ptrdiff_t>(buffer.StrideU()) * chromaHeight;
    };
    if (!isTightlyPacked(*i420Buffer))
        i420Buffer = webrtc::I420Buffer::Copy(*i420Buffer);

    int width = i420Buffer->width();
    int height = i420Buffer->height();
    int chromaHeight = (height + 1) / 2;
    gint strides[3] = { i420Buffer->StrideY(), i420Buffer->StrideU(), i420Buffer->StrideV() };
    gsize offsets[3] = {
        0,
        static_cast<gsize>(i420Buffer->DataU() - i420Buffer->DataY()),
        static_cast<gsize>(i420Buffer->DataV() - i420Buffer->DataY())
    };
    gsize size = offsets[2] + static_cast<gsize>(strides[2]) * chromaHeight;

    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_I420, width, height);
    if (auto colorSpace = colorSpaceFromLibWebRTCVideoFrame(frame))
        fillVideoInfoColorimetryFromColorSpace(&info, *colorSpace);

    auto* data = const_cast<uint8_t*>(i420Buffer->DataY());
    auto* holder = i420Buffer.release();
    GRefPtr buffer = adoptGRef(gst_buffer_new_wrapped_full(static_cast<GstMemoryFlags>(GST_MEMORY_FLAG_NO_SHARE | GST_MEMORY_FLAG_READONLY), data, size, 0, size, holder, [](gpointer holder) {
        static_cast<webrtc::I420BufferInterface*>(holder)->Release();
    }));
    gst_buffer_add_video_meta_full(buffer.get(), GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420, width, height, 3, offsets, strides);
    GST_BUFFER_PTS(buffer.get()) = toGstClockTime(WTF::MediaTime(frame.render_time_ms(), G_USEC_PER_SEC * 1000));

    GRefPtr caps = adoptGRef(gst_video_info_to_caps(&info));
    return adoptGRef(gst_sample_new(buffer.get(), caps.get(), nullptr, nullptr));
}

std::optional<webrtc::VideoFrame> convertGStreamerSampleToLibWebRTCVideoFrame(GRefPtr<GstSample>&& sample, uint32_t rtpTimestamp)
{
    auto buffer = gst_sample_get_buffer(sample.get());
    if (!buffer) [[unlikely]]
        return std::nullopt;

    auto pts = GST_BUFFER_PTS(buffer);
    auto frameBuffer = GStreamerVideoFrameLibWebRTC::create(WTF::move(sample));
    if (!frameBuffer)
        return std::nullopt;

    webrtc::VideoFrame::Builder builder;
    builder.set_video_frame_buffer(frameBuffer).set_rtp_timestamp(rtpTimestamp);
    if (GST_CLOCK_TIME_IS_VALID(pts))
        builder.set_timestamp_us(GST_TIME_AS_USECONDS(pts));
    return builder.build();
}

GStreamerVideoFrameLibWebRTC::GStreamerVideoFrameLibWebRTC(GRefPtr<GstSample>&& sample, MemoryType memoryType, IntSize sourceSize, FloatRect visibleRect, IntSize scaledSize)
    : m_sample(WTF::move(sample))
    , m_memoryType(memoryType)
    , m_sourceSize(sourceSize)
    , m_visibleRect(visibleRect)
    , m_scaledSize(scaledSize)
{
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GStreamerVideoFrameLibWebRTC::create(GRefPtr<GstSample>&& sample)
{
    ensureDebugCategoryIsRegistered();
    auto* caps = gst_sample_get_caps(sample.get());
    auto size = sampleSize(caps);
    if (!size) {
        GST_WARNING("Unable to wrap a sample with caps %" GST_PTR_FORMAT, caps);
        return nullptr;
    }
    return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(new GStreamerVideoFrameLibWebRTC(WTF::move(sample), memoryTypeFromCaps(caps), *size, FloatRect { {}, *size }, *size));
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GStreamerVideoFrameLibWebRTC::createScaled(GRefPtr<GstSample>&& sample, IntSize scaledSize)
{
    ensureDebugCategoryIsRegistered();
    auto* caps = gst_sample_get_caps(sample.get());
    auto size = sampleSize(caps);
    if (!size || scaledSize.isEmpty()) {
        GST_WARNING("Unable to wrap a sample with caps %" GST_PTR_FORMAT " at %dx%d", caps, scaledSize.width(), scaledSize.height());
        return nullptr;
    }
    return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(new GStreamerVideoFrameLibWebRTC(WTF::move(sample), memoryTypeFromCaps(caps), *size, FloatRect { {}, *size }, scaledSize));
}

bool GStreamerVideoFrameLibWebRTC::isCropped() const
{
    return m_visibleRect != FloatRect { {}, m_sourceSize };
}

bool GStreamerVideoFrameLibWebRTC::isScaled() const
{
    return m_scaledSize != m_sourceSize;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GStreamerVideoFrameLibWebRTC::CropAndScale(int offsetX, int offsetY, int cropWidth, int cropHeight, int scaledWidth, int scaledHeight)
{
    if ((m_memoryType != MemoryType::DMABuf && m_memoryType != MemoryType::GL) || cropWidth <= 0 || cropHeight <= 0 || scaledWidth <= 0 || scaledHeight <= 0)
        return webrtc::VideoFrameBuffer::CropAndScale(offsetX, offsetY, cropWidth, cropHeight, scaledWidth, scaledHeight);

    float scaleX = m_visibleRect.width() / m_scaledSize.width();
    float scaleY = m_visibleRect.height() / m_scaledSize.height();
    FloatRect visibleRect { m_visibleRect.x() + offsetX * scaleX, m_visibleRect.y() + offsetY * scaleY, cropWidth * scaleX, cropHeight * scaleY };
    return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(new GStreamerVideoFrameLibWebRTC(GRefPtr(m_sample), m_memoryType, m_sourceSize, visibleRect, { scaledWidth, scaledHeight }));
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> GStreamerVideoFrameLibWebRTC::copyToI420()
{
    auto* caps = gst_sample_get_caps(m_sample.get());
    IntSize convertedSize = m_scaledSize;
    IntPoint offset;
    if (isCropped()) {
        float scaleX = m_scaledSize.width() / m_visibleRect.width();
        float scaleY = m_scaledSize.height() / m_visibleRect.height();
        if (scaleX > 2 || scaleY > 2) {
            auto sourceBuffer = webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(new GStreamerVideoFrameLibWebRTC(GRefPtr(m_sample), m_memoryType, m_sourceSize, FloatRect { {}, m_sourceSize }, m_sourceSize));
            auto source = sourceBuffer->ToI420();
            if (!source)
                return nullptr;
            auto scaled = webrtc::I420Buffer::Create(m_scaledSize.width(), m_scaledSize.height());
            scaled->CropAndScaleFrom(*source, std::lround(m_visibleRect.x()), std::lround(m_visibleRect.y()), std::lround(m_visibleRect.width()), std::lround(m_visibleRect.height()));
            return scaled;
        }
        convertedSize = { std::max<int>(m_scaledSize.width(), std::lround(m_sourceSize.width() * scaleX)), std::max<int>(m_scaledSize.height(), std::lround(m_sourceSize.height() * scaleY)) };
        int x = std::lround(m_visibleRect.x() * scaleX) & ~1;
        int y = std::lround(m_visibleRect.y() * scaleY) & ~1;
        offset = { std::clamp(x, 0, (convertedSize.width() - m_scaledSize.width()) & ~1), std::clamp(y, 0, (convertedSize.height() - m_scaledSize.height()) & ~1) };
    }

    GRefPtr<GstSample> i420Sample;
    auto* structure = gst_caps_get_structure(caps, 0);
    if (m_memoryType == MemoryType::System && convertedSize == m_sourceSize && !g_strcmp0(gst_structure_get_string(structure, "format"), "I420"))
        i420Sample = m_sample;
    else {
        GRefPtr outputCaps = adoptGRef(gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", "width", G_TYPE_INT, convertedSize.width(), "height", G_TYPE_INT, convertedSize.height(), nullptr));
        if (isRGBSample(caps))
            gst_caps_set_simple(outputCaps.get(), "colorimetry", G_TYPE_STRING, "bt709", nullptr);
        GST_TRACE("Converting a %dx%d frame to %dx%d I420", m_sourceSize.width(), m_sourceSize.height(), convertedSize.width(), convertedSize.height());
        i420Sample = GStreamerVideoFrameConverter::singleton().convert(m_sample, outputCaps);
    }
    if (!i420Sample) [[unlikely]]
        return nullptr;
    return wrapI420Sample(WTF::move(i420Sample), offset, m_scaledSize);
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> GStreamerVideoFrameLibWebRTC::ToI420()
{
    ensureDebugCategoryIsRegistered();
    Locker locker { m_i420Lock };
    if (!m_i420Buffer)
        m_i420Buffer = copyToI420();
    if (m_i420Buffer)
        return m_i420Buffer;

    GST_WARNING("Unable to convert a %dx%d frame to I420, substituting a black frame", width(), height());
    auto blackBuffer = webrtc::I420Buffer::Create(std::max(width(), 2), std::max(height(), 2));
    webrtc::I420Buffer::SetBlack(blackBuffer.get());
    return blackBuffer;
}

} // namespace WebCore

#undef GST_CAT_DEFAULT

#endif // USE(LIBWEBRTC) && USE(GSTREAMER)
