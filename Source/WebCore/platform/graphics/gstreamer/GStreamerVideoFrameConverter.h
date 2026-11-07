/*
 *  Copyright (C) 2025 Igalia, S.L
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

#pragma once

#if ENABLE(VIDEO) && USE(GSTREAMER)

#include "GRefPtrGStreamer.h"
#include <wtf/Forward.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/Vector.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

class GStreamerVideoFrameConverter final : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<GStreamerVideoFrameConverter> {
    WTF_MAKE_TZONE_ALLOCATED(GStreamerVideoFrameConverter);
    friend NeverDestroyed<GStreamerVideoFrameConverter>;

public:
    static GStreamerVideoFrameConverter& singleton();

    // Do nothing since this is a singleton object.
    void ref() const { }
    void deref() const { }

    [[nodiscard]] GRefPtr<GstSample> convert(const GRefPtr<GstSample>&, const GRefPtr<GstCaps>&);

private:
    GStreamerVideoFrameConverter();

    class Pipeline {
        WTF_MAKE_TZONE_ALLOCATED(Pipeline);
    public:
        Pipeline();
        ~Pipeline();

        GRefPtr<GstSample> run(const GRefPtr<GstSample>&, GstCaps*);

    private:
        GRefPtr<GstElement> m_pipeline;
        GRefPtr<GstElement> m_src;
        GRefPtr<GstElement> m_sink;
    };

#if USE(GSTREAMER_GL)
    class GPUPipeline {
        WTF_MAKE_TZONE_ALLOCATED(GPUPipeline);

    public:
        static std::unique_ptr<GPUPipeline> create(GRefPtr<GstCaps>&& inputCaps, GRefPtr<GstCaps>&& outputCaps);
        ~GPUPipeline();

        bool matches(GstCaps* inputCaps, GstCaps* outputCaps) const;
        bool isUsable() const { return m_isUsable; }
        MonotonicTime failureTime() const { return m_failureTime; }
        GRefPtr<GstSample> run(GstBuffer*);

        MonotonicTime lastUse;

    private:
        GPUPipeline(GRefPtr<GstCaps>&&, GRefPtr<GstCaps>&&);
        bool initialize();
        void tearDown();

        GRefPtr<GstCaps> m_inputCaps;
        GRefPtr<GstCaps> m_outputCaps;
        GRefPtr<GstElement> m_pipeline;
        GRefPtr<GstElement> m_sink;
        GRefPtr<GstPad> m_sourcePad;
        bool m_isUsable { false };
        MonotonicTime m_failureTime;
    };

    GRefPtr<GstSample> convertOnGPU(const GRefPtr<GstSample>&, GstCaps*) WTF_REQUIRES_LOCK(m_lock);
#endif

    Pipeline& ensurePipeline() WTF_REQUIRES_LOCK(m_lock);
    void scheduleReleaseOfUnusedPipelines();
    void releaseUnusedPipelinesTimerFired();

    Lock m_lock;
    std::unique_ptr<Pipeline> m_systemMemoryPipeline WTF_GUARDED_BY_LOCK(m_lock);
    MonotonicTime m_systemMemoryPipelineLastUse WTF_GUARDED_BY_LOCK(m_lock);
#if USE(GSTREAMER_GL)
    Vector<std::unique_ptr<GPUPipeline>> m_gpuPipelines WTF_GUARDED_BY_LOCK(m_lock);
#endif
    RunLoop::Timer m_releaseUnusedPipelinesTimer;
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER)
