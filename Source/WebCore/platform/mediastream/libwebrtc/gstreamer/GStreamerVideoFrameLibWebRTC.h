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

#pragma once

#if USE(GSTREAMER) && USE(LIBWEBRTC)

#include "FloatRect.h"
#include "GStreamerCommon.h"
#include "IntSize.h"
#include "LibWebRTCMacros.h"
#include "webrtc/api/video/i420_buffer.h"
#include "webrtc/api/video/video_frame.h"
#include "webrtc/common_video/include/video_frame_buffer.h"
#include "webrtc/rtc_base/ref_counted_object.h"
#include <optional>
#include <wtf/Lock.h>

namespace WebCore {

[[nodiscard]] GRefPtr<GstSample> convertLibWebRTCVideoFrameToGStreamerSample(const webrtc::VideoFrame&, bool acceptsDMABuf = false);

std::optional<webrtc::VideoFrame> convertGStreamerSampleToLibWebRTCVideoFrame(GRefPtr<GstSample>&&, uint32_t rtpTimestamp);

class GStreamerVideoFrameLibWebRTC : public webrtc::RefCountedObject<webrtc::VideoFrameBuffer> {
public:
    enum class MemoryType : uint8_t { System,
        DMABuf,
        GL,
        Other };

    static webrtc::scoped_refptr<webrtc::VideoFrameBuffer> create(GRefPtr<GstSample>&&);
    static webrtc::scoped_refptr<webrtc::VideoFrameBuffer> createScaled(GRefPtr<GstSample>&&, IntSize);

    const GRefPtr<GstSample>& sample() const LIFETIME_BOUND { return m_sample; }
    MemoryType memoryType() const { return m_memoryType; }
    bool isCropped() const;
    bool isScaled() const;

    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() final;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(int offsetX, int offsetY, int cropWidth, int cropHeight, int scaledWidth, int scaledHeight) final;
    int width() const final { return m_scaledSize.width(); }
    int height() const final { return m_scaledSize.height(); }

private:
    GStreamerVideoFrameLibWebRTC(GRefPtr<GstSample>&&, MemoryType, IntSize sourceSize, FloatRect visibleRect, IntSize scaledSize);
    webrtc::VideoFrameBuffer::Type type() const final { return Type::kNative; }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> copyToI420();

    GRefPtr<GstSample> m_sample;
    MemoryType m_memoryType;
    IntSize m_sourceSize;
    FloatRect m_visibleRect;
    IntSize m_scaledSize;
    Lock m_i420Lock;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> m_i420Buffer WTF_GUARDED_BY_LOCK(m_i420Lock);
};

} // namespace WebCore

#endif // USE(GSTREAMER) && USE(LIBWEBRTC)
