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

#pragma once

#if ENABLE(MEDIA_STREAM) && USE(GSTREAMER)

#include "GStreamerCapturer.h"
#include "IntSize.h"
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace WebCore {

class VideoFrameGStreamer;

class PipeWireFrameGate final : public ThreadSafeRefCounted<PipeWireFrameGate> {
public:
    static Ref<PipeWireFrameGate> create() { return adoptRef(*new PipeWireFrameGate); }

    void release(GRefPtr<GstBuffer>&& buffer)
    {
        if (m_closeLock.tryLock()) {
            Locker locker { AdoptLock, m_closeLock };
            buffer = nullptr;
            return;
        }
        Locker locker { m_pendingLock };
        m_pending.append(WTF::move(buffer));
    }

    void close(NOESCAPE const Function<void()>& closeSource)
    {
        Locker locker { m_closeLock };
        closeSource();
        Vector<GRefPtr<GstBuffer>> pending;
        {
            Locker pendingLocker { m_pendingLock };
            pending = std::exchange(m_pending, {});
        }
    }

private:
    PipeWireFrameGate() = default;

    Lock m_closeLock;
    Lock m_pendingLock;
    Vector<GRefPtr<GstBuffer>> m_pending WTF_GUARDED_BY_LOCK(m_pendingLock);
};

class GStreamerVideoCapturer final : public GStreamerCapturer {
    friend class GStreamerVideoCaptureSource;
    friend class MockRealtimeVideoSourceGStreamer;
public:
    GStreamerVideoCapturer(GStreamerCaptureDevice&&);
    GStreamerVideoCapturer(const PipeWireCaptureDevice&);
    ~GStreamerVideoCapturer() = default;

    void tearDown(bool disconnectSignals) final;
    void setupPipeline() final;
    GstElement* createConverter() final;
    ASCIILiteral name() final { return "Video"_s; }

    using SinkVideoFrameCallback = Function<void(Ref<VideoFrameGStreamer>&&)>;
    void setSinkVideoFrameCallback(SinkVideoFrameCallback&&);

private:
    void handleSample(GRefPtr<GstSample>&&);
    void updateDisplayFrameSize(const IntSize&, const GRefPtr<GstSample>&);
    bool setSize(const IntSize&);
    const IntSize& size() const LIFETIME_BOUND { return m_size; }

    bool setFrameRate(double);
    void reconfigure();

    bool isCapturingDisplay() const;

    GRefPtr<GstElement> m_videoSrcMIMETypeFilter;

    std::pair<GStreamerCapturer::SinkSignalsHolder, SinkVideoFrameCallback> m_sinkVideoFrameCallback;
    IntSize m_size;
    IntSize m_displaySourceSize;
    IntSize m_displayFrameSize;
    RefPtr<PipeWireFrameGate> m_frameGate;
    GRefPtr<GstElement> m_aspectRatioCrop;
};

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
