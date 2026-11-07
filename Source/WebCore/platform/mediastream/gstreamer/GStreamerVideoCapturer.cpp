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
#include "GStreamerVideoCapturer.h"

#include "GStreamerVideoFrameConverter.h"
#include "VideoFrameGStreamer.h"
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <tuple>

GST_DEBUG_CATEGORY(webkit_video_capturer_debug);
#define GST_CAT_DEFAULT webkit_video_capturer_debug

namespace WebCore {

static void initializeVideoCapturerDebugCategory()
{
    ensureGStreamerInitialized();

    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_video_capturer_debug, "webkitvideocapturer", 0, "WebKit Video Capturer");
    });
}

GStreamerVideoCapturer::GStreamerVideoCapturer(GStreamerCaptureDevice&& device)
    : GStreamerCapturer(WTF::move(device), adoptGRef(gst_caps_from_string("video/x-raw, format=(string)I420, pixel-aspect-ratio=(fraction)1/1")))
{
    initializeVideoCapturerDebugCategory();
}

GStreamerVideoCapturer::GStreamerVideoCapturer(const PipeWireCaptureDevice& device)
    : GStreamerCapturer(device)
    , m_frameGate(PipeWireFrameGate::create())
{
    initializeVideoCapturerDebugCategory();
}

static GQuark pipeWireFrameGateQuark()
{
    static GQuark quark = g_quark_from_static_string("webkit-pipewire-frame-gate");
    return quark;
}

struct GatedPipeWireBuffer {
    Ref<PipeWireFrameGate> gate;
    GRefPtr<GstBuffer> buffer;
};

static GRefPtr<GstSample> sampleReleasedThroughGate(GRefPtr<GstSample>&& sample, PipeWireFrameGate& gate)
{
    auto* buffer = gst_sample_get_buffer(sample.get());
    if (!buffer)
        return WTF::move(sample);

    GRefPtr gated = adoptGRef(gst_buffer_copy_region(buffer, static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META | GST_BUFFER_COPY_MEMORY), 0, static_cast<gsize>(-1)));
    Vector<GstMeta*> parents;
    gpointer state = nullptr;
    while (auto* meta = gst_buffer_iterate_meta_filtered(gated.get(), &state, GST_PARENT_BUFFER_META_API_TYPE))
        parents.append(meta);
    for (auto* parent : parents)
        gst_buffer_remove_meta(gated.get(), parent);

    gst_mini_object_set_qdata(GST_MINI_OBJECT_CAST(gated.get()), pipeWireFrameGateQuark(), new GatedPipeWireBuffer { gate, GRefPtr<GstBuffer>(buffer) }, [](gpointer data) {
        std::unique_ptr<GatedPipeWireBuffer> gatedBuffer(static_cast<GatedPipeWireBuffer*>(data));
        gatedBuffer->gate->release(WTF::move(gatedBuffer->buffer));
    });
    sample = adoptGRef(gst_sample_make_writable(sample.leakRef()));
    gst_sample_set_buffer(sample.get(), gated.get());
    return WTF::move(sample);
}

static GRefPtr<GstSample> sampleWithVideoMeta(GRefPtr<GstSample>&& sample)
{
    auto* buffer = gst_sample_get_buffer(sample.get());
    auto* caps = gst_sample_get_caps(sample.get());
    if (!buffer || !caps || gst_buffer_get_video_meta(buffer))
        return WTF::move(sample);

    GstVideoInfo info;
#if GST_CHECK_VERSION(1, 24, 0)
    if (gst_video_is_dma_drm_caps(caps)) {
        GstVideoInfoDmaDrm drmInfo;
        if (!gst_video_info_dma_drm_from_caps(&drmInfo, caps) || !gst_video_info_dma_drm_to_video_info(&drmInfo, &info))
            return WTF::move(sample);
    } else
#endif
        if (!gst_video_info_from_caps(&info, caps))
        return WTF::move(sample);

    GRefPtr writableBuffer = adoptGRef(gst_buffer_make_writable(gst_buffer_ref(buffer)));
    gst_buffer_add_parent_buffer_meta(writableBuffer.get(), buffer);
    gst_buffer_add_video_meta_full(writableBuffer.get(), GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_INFO_FORMAT(&info), GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info), GST_VIDEO_INFO_N_PLANES(&info), info.offset, info.stride);
    sample = adoptGRef(gst_sample_make_writable(sample.leakRef()));
    gst_sample_set_buffer(sample.get(), writableBuffer.get());
    return WTF::move(sample);
}

static IntSize sampleResolution(const GRefPtr<GstSample>& sample)
{
    auto resolution = getVideoResolutionFromCaps(gst_sample_get_caps(sample.get()));
    return resolution ? IntSize(*resolution) : IntSize {};
}

static GRefPtr<GstSample> sampleWithCropRectangle(const GRefPtr<GstSample>& sample, const GstVideoInfo& info, GstBuffer* timingSource, unsigned x, unsigned y, unsigned width, unsigned height)
{
    auto* sourceBuffer = gst_sample_get_buffer(sample.get());
    GRefPtr writableBuffer = adoptGRef(gst_buffer_make_writable(gst_buffer_ref(sourceBuffer)));
    gst_buffer_add_parent_buffer_meta(writableBuffer.get(), sourceBuffer);
    GST_BUFFER_PTS(writableBuffer.get()) = GST_BUFFER_PTS(timingSource);
    GST_BUFFER_DTS(writableBuffer.get()) = GST_BUFFER_DTS(timingSource);
    GST_BUFFER_DURATION(writableBuffer.get()) = GST_BUFFER_DURATION(timingSource);

    auto* videoMeta = gst_buffer_get_video_meta(writableBuffer.get());
    unsigned planes = GST_VIDEO_INFO_N_PLANES(&info);
    gsize offsets[GST_VIDEO_MAX_PLANES] = {};
    gint strides[GST_VIDEO_MAX_PLANES] = {};
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN;
    for (unsigned plane = 0; plane < planes; ++plane) {
        offsets[plane] = videoMeta ? videoMeta->offset[plane] : GST_VIDEO_INFO_PLANE_OFFSET(&info, plane);
        strides[plane] = videoMeta ? videoMeta->stride[plane] : GST_VIDEO_INFO_PLANE_STRIDE(&info, plane);
        gint components[GST_VIDEO_MAX_COMPONENTS];
        gst_video_format_info_component(info.finfo, plane, components);
        auto componentX = GST_VIDEO_FORMAT_INFO_SCALE_WIDTH(info.finfo, components[0], x);
        auto componentY = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(info.finfo, components[0], y);
        offsets[plane] += static_cast<gsize>(componentY) * strides[plane] + static_cast<gsize>(componentX) * GST_VIDEO_FORMAT_INFO_PSTRIDE(info.finfo, components[0]);
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;
    auto flags = videoMeta ? videoMeta->flags : GST_VIDEO_FRAME_FLAG_NONE;
    if (videoMeta)
        gst_buffer_remove_meta(writableBuffer.get(), GST_META_CAST(videoMeta));
    if (auto* cropMeta = gst_buffer_get_video_crop_meta(writableBuffer.get()))
        gst_buffer_remove_meta(writableBuffer.get(), GST_META_CAST(cropMeta));
    gst_buffer_add_video_meta_full(writableBuffer.get(), flags, GST_VIDEO_INFO_FORMAT(&info), width, height, planes, offsets, strides);

    GRefPtr caps = adoptGRef(gst_caps_copy(gst_sample_get_caps(sample.get())));
    gst_caps_set_simple(caps.get(), "width", G_TYPE_INT, static_cast<int>(width), "height", G_TYPE_INT, static_cast<int>(height), nullptr);
    return adoptGRef(gst_sample_new(writableBuffer.get(), caps.get(), gst_sample_get_segment(sample.get()), nullptr));
}

static GRefPtr<GstSample> sampleWithCropApplied(GRefPtr<GstSample>&& sample)
{
    auto* buffer = gst_sample_get_buffer(sample.get());
    auto* caps = gst_sample_get_caps(sample.get());
    auto* crop = buffer && caps ? gst_buffer_get_video_crop_meta(buffer) : nullptr;
    if (!crop)
        return WTF::move(sample);

    GstVideoInfo info;
    bool isLinear = true;
#if GST_CHECK_VERSION(1, 24, 0)
    if (gst_video_is_dma_drm_caps(caps)) {
        GstVideoInfoDmaDrm drmInfo;
        if (!gst_video_info_dma_drm_from_caps(&drmInfo, caps) || !gst_video_info_dma_drm_to_video_info(&drmInfo, &info))
            return WTF::move(sample);
        isLinear = !drmInfo.drm_modifier;
    } else
#endif
        if (!gst_video_info_from_caps(&info, caps))
        return WTF::move(sample);

    unsigned frameWidth = GST_VIDEO_INFO_WIDTH(&info);
    unsigned frameHeight = GST_VIDEO_INFO_HEIGHT(&info);
    if (!crop->width || !crop->height || crop->x + crop->width > frameWidth || crop->y + crop->height > frameHeight) {
        GST_WARNING("Ignoring a %ux%u crop at %u,%u outside a %ux%u frame", crop->width, crop->height, crop->x, crop->y, frameWidth, frameHeight);
        return WTF::move(sample);
    }

    unsigned alignmentX = 1;
    unsigned alignmentY = 1;
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN;
    for (unsigned component = 0; component < GST_VIDEO_INFO_N_COMPONENTS(&info); ++component) {
        alignmentX = std::max(alignmentX, 1u << GST_VIDEO_FORMAT_INFO_W_SUB(info.finfo, component));
        alignmentY = std::max(alignmentY, 1u << GST_VIDEO_FORMAT_INFO_H_SUB(info.finfo, component));
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;
    unsigned x = crop->x - crop->x % alignmentX;
    unsigned y = crop->y - crop->y % alignmentY;
    unsigned width = crop->width + crop->x - x;
    unsigned height = crop->height + crop->y - y;

    if (isLinear && !GST_VIDEO_FORMAT_INFO_IS_TILED(info.finfo))
        return sampleWithCropRectangle(sample, info, buffer, x, y, width, height);

    GRefPtr systemMemoryCaps = adoptGRef(gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", "width", G_TYPE_INT, static_cast<int>(frameWidth), "height", G_TYPE_INT, static_cast<int>(frameHeight), nullptr));
    auto converted = GStreamerVideoFrameConverter::singleton().convert(sample, systemMemoryCaps);
    GstVideoInfo convertedInfo;
    if (!converted || !gst_video_info_from_caps(&convertedInfo, gst_sample_get_caps(converted.get()))) {
        GST_WARNING("Could not bring a %ux%u frame into system memory to crop it", frameWidth, frameHeight);
        return WTF::move(sample);
    }
    return sampleWithCropRectangle(converted, convertedInfo, buffer, crop->x, crop->y, crop->width, crop->height);
}

void GStreamerVideoCapturer::updateDisplayFrameSize(const IntSize& sourceSize, const GRefPtr<GstSample>& sample)
{
    auto frameSize = sampleResolution(sample);
    if (frameSize.isEmpty() || (frameSize == m_displayFrameSize && sourceSize == m_displaySourceSize))
        return;

    m_displayFrameSize = frameSize;
    m_displaySourceSize = sourceSize;
    GST_DEBUG_OBJECT(pipeline(), "Display frames are %dx%d inside %dx%d buffers", frameSize.width(), frameSize.height(), sourceSize.width(), sourceSize.height());
    callOnMainThread([weakThis = ThreadSafeWeakPtr<GStreamerCapturer> { *this }, caps = GRefPtr<GstCaps>(gst_sample_get_caps(sample.get()))] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        protectedThis->forEachObserver([&caps](auto& observer) {
            observer.sourceCapsChanged(caps.get());
        });
    });
}

void GStreamerVideoCapturer::handleSample(GRefPtr<GstSample>&& capturedSample)
{
    auto sourceSize = isCapturingDisplay() ? sampleResolution(capturedSample) : IntSize {};
    if (m_frameGate)
        capturedSample = sampleReleasedThroughGate(WTF::move(capturedSample), *m_frameGate);
    auto sample = sampleWithCropApplied(sampleWithVideoMeta(WTF::move(capturedSample)));
    if (isCapturingDisplay())
        updateDisplayFrameSize(sourceSize, sample);
    VideoFrameTimeMetadata metadata;
    metadata.captureTime = MonotonicTime::now().secondsSinceEpoch();

    auto buffer = gst_sample_get_buffer(sample.get());
    MediaTime presentationTime = MediaTime::invalidTime();
    if (GST_BUFFER_PTS_IS_VALID(buffer))
        presentationTime = fromGstClockTime(GST_BUFFER_PTS(buffer));

    auto rotationFromMeta = webkitGstBufferGetVideoRotation(buffer);
    auto size = isCapturingDisplay() ? sampleResolution(sample) : this->size();
    VideoFrameGStreamer::CreateOptions options(WTF::move(size));
    options.presentationTime = presentationTime;
    options.rotation = rotationFromMeta.first;
    options.isMirrored = rotationFromMeta.second;
    options.timeMetadata = WTF::move(metadata);
    m_sinkVideoFrameCallback.second(VideoFrameGStreamer::create(WTF::move(sample), options));
}

void GStreamerVideoCapturer::setSinkVideoFrameCallback(SinkVideoFrameCallback&& callback)
{
    if (m_sinkVideoFrameCallback.first.newSampleSignalId)
        g_signal_handler_disconnect(sink(), m_sinkVideoFrameCallback.first.newSampleSignalId);

    m_sinkVideoFrameCallback.second = WTF::move(callback);
    m_sinkVideoFrameCallback.first.newSampleSignalId = g_signal_connect_swapped(sink(), "new-sample", G_CALLBACK(+[](GStreamerVideoCapturer* capturer, GstElement* sink) -> GstFlowReturn {
        GRefPtr sample = adoptGRef(gst_app_sink_pull_sample(GST_APP_SINK(sink)));
        capturer->handleSample(WTF::move(sample));
        return GST_FLOW_OK;
    }), this);
}

bool GStreamerVideoCapturer::isCapturingDisplay() const
{
    auto deviceType = this->deviceType();
    return deviceType == CaptureDevice::DeviceType::Screen || deviceType == CaptureDevice::DeviceType::Window;
}

void GStreamerVideoCapturer::tearDown(bool disconnectSignals)
{
    if (RefPtr frameGate = m_frameGate) {
        frameGate->close([&] {
            GStreamerCapturer::tearDown(disconnectSignals);
        });
    } else
        GStreamerCapturer::tearDown(disconnectSignals);
    if (disconnectSignals) {
        m_videoSrcMIMETypeFilter = nullptr;
        m_aspectRatioCrop = nullptr;
    }
}

void GStreamerVideoCapturer::setupPipeline() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    GStreamerCapturer::setupPipeline();
    g_object_set(m_sink.get(), "sync", FALSE, nullptr);

    // Without buffer pool, pipewiresrc will copy its DMABuf memories instead of sharing them.
    if (isCapturingDisplay() && gstElementMatchesFactoryAndHasProperty(m_src.get(), "pipewiresrc"_s, "use-bufferpool"_s))
        g_object_set(m_src.get(), "use-bufferpool", TRUE, nullptr);

    GRefPtr pad = adoptGRef(gst_element_get_static_pad(m_sink.get(), "sink"));
    gst_pad_add_probe(pad.get(), GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, reinterpret_cast<GstPadProbeCallback>(+[](GstPad*, GstPadProbeInfo* info, gpointer) -> GstPadProbeReturn {
        if (GST_QUERY_TYPE(GST_PAD_PROBE_INFO_QUERY(info)) == GST_QUERY_ALLOCATION)
            gst_query_add_allocation_meta(GST_PAD_PROBE_INFO_QUERY(info), GST_VIDEO_META_API_TYPE, nullptr);
        return GST_PAD_PROBE_OK;
    }), nullptr, nullptr);
}

GstElement* GStreamerVideoCapturer::createConverter()
{
    if (isCapturingDisplay())
        return nullptr;

    auto videoConvert = createVideoConvertScaleElement();
    if (!videoConvert) [[unlikely]]
        return nullptr;

    auto* bin = gst_bin_new(nullptr);
    auto* videorate = makeGStreamerElement("videorate"_s, "videorate"_s);

    // The workaround below doesn't seem necessary anymore in GStreamer 1.28 and beyond.
    // Fixed by: https://gitlab.freedesktop.org/gstreamer/gstreamer/-/commit/6f623af4d745efaacd0c8639b99536def4a65c78
    g_object_set(videorate, "drop-only", TRUE, "average-period", UINT64_C(1), nullptr);

    gst_bin_add_many(GST_BIN_CAST(bin), videoConvert.get(), videorate, nullptr);

    m_videoSrcMIMETypeFilter = gst_element_factory_make("capsfilter", "mimetype-filter");

    GRefPtr caps = adoptGRef(gst_caps_new_empty_simple("video/x-raw"));
    g_object_set(m_videoSrcMIMETypeFilter.get(), "caps", caps.get(), nullptr);

    auto* decodebin = makeGStreamerElement("decodebin3"_s);
    gst_bin_add_many(GST_BIN_CAST(bin), m_videoSrcMIMETypeFilter.get(), decodebin, nullptr);
    gst_element_link(m_videoSrcMIMETypeFilter.get(), decodebin);

    m_aspectRatioCrop = gst_element_factory_make("aspectratiocrop", nullptr);
    GRefPtr<GstElement> decodedVideoSink = videoConvert;
    if (m_aspectRatioCrop) {
        gst_bin_add(GST_BIN_CAST(bin), m_aspectRatioCrop.get());
        gst_element_link(m_aspectRatioCrop.get(), videoConvert.get());
        decodedVideoSink = m_aspectRatioCrop;
    } else
        GST_WARNING("aspectratiocrop is unavailable, camera frames are scaled without cropping");

    GRefPtr sinkPad = adoptGRef(gst_element_get_static_pad(decodedVideoSink.get(), "sink"));

    g_signal_connect_data(decodebin, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* srcPad, GstPad* sinkPad) {
        RELEASE_ASSERT(!gst_pad_is_linked(sinkPad));
        gst_pad_link(srcPad, sinkPad);
    }), sinkPad.leakRef(), reinterpret_cast<GClosureNotify>(+[](gpointer data, GClosure*) {
        gst_object_unref(GST_PAD_CAST(data));
    }), static_cast<GConnectFlags>(0));

    gst_element_link(videoConvert.get(), videorate);

    sinkPad = adoptGRef(gst_element_get_static_pad(m_videoSrcMIMETypeFilter.get(), "sink"));
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", sinkPad.get()));

    GRefPtr srcPad = adoptGRef(gst_element_get_static_pad(videorate, "src"));
    gst_element_add_pad(bin, gst_ghost_pad_new("src", srcPad.get()));

    return bin;
}

bool GStreamerVideoCapturer::setSize(const IntSize& size)
{
    if (isCapturingDisplay()) {
        // Pipewiresrc doesn't seem to support caps re-negotiation and framerate configuration properly.
        GST_FIXME_OBJECT(m_pipeline.get(), "Resizing disabled on display capture source");
        return true;
    }

    int width = size.width();
    int height = size.height();
    GST_INFO_OBJECT(m_pipeline.get(), "Setting size to %dx%d", width, height);
    if (!width || !height)
        return false;

    auto videoResolution = getVideoResolutionFromCaps(m_caps.get());
    if (videoResolution && videoResolution->width() == width && videoResolution->height() == height) {
        GST_DEBUG_OBJECT(m_pipeline.get(), "Size has not changed");
        return true;
    }

    if (!m_capsfilter) [[unlikely]]
        return false;

    m_size = size;
    GRefPtr modifiedCaps = adoptGRef(gst_caps_make_writable(m_caps.leakRef()));
    gst_caps_set_simple(modifiedCaps.get(), "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, nullptr);
    gst_caps_take(&m_caps.outPtr(), modifiedCaps.leakRef());

    g_object_set(m_capsfilter.get(), "caps", m_caps.get(), nullptr);
    return true;
}

bool GStreamerVideoCapturer::setFrameRate(double frameRate)
{
    if (isCapturingDisplay()) {
        // Pipewiresrc doesn't seem to support caps re-negotiation and framerate configuration properly.
        GST_FIXME_OBJECT(m_pipeline.get(), "Framerate override disabled on display capture source");
        return true;
    }

    int numerator, denominator;
    gst_util_double_to_fraction(frameRate, &numerator, &denominator);

    if (numerator < -G_MAXINT) {
        GST_INFO_OBJECT(m_pipeline.get(), "Framerate %f not allowed", frameRate);
        return false;
    }

    if (!numerator) {
        GST_INFO_OBJECT(m_pipeline.get(), "Do not force variable framerate");
        return false;
    }

    if (!m_capsfilter) [[unlikely]]
        return false;

    GRefPtr modifiedCaps = adoptGRef(gst_caps_make_writable(m_caps.leakRef()));
    gst_caps_set_simple(modifiedCaps.get(), "framerate", GST_TYPE_FRACTION, numerator, denominator, nullptr);
    gst_caps_take(&m_caps.outPtr(), modifiedCaps.leakRef());

    GST_INFO_OBJECT(m_pipeline.get(), "Setting framerate to %f fps", frameRate);
    g_object_set(m_capsfilter.get(), "caps", m_caps.get(), nullptr);
    return true;
}

static std::optional<int> getMaxIntValueFromStructure(const GstStructure* structure, ASCIILiteral fieldName)
{
    const GValue* value = gst_structure_get_value(structure, fieldName.characters());
    if (!value)
        return std::nullopt;

    int maxInt = -G_MAXINT;
    if (G_VALUE_HOLDS_INT(value))
        maxInt = g_value_get_int(value);
    else if (GST_VALUE_HOLDS_INT_RANGE(value))
        maxInt = gst_value_get_int_range_max(value);
    else if (GST_VALUE_HOLDS_ARRAY(value)) {
        const guint size = gst_value_array_get_size(value);
        for (guint i = 0; i < size; ++i) {
            const GValue* item = gst_value_array_get_value(value, i);
            if (G_VALUE_HOLDS_INT(item)) {
                int val = g_value_get_int(item);
                if (val > maxInt)
                    maxInt = val;
            }
        }
    } else if (GST_VALUE_HOLDS_LIST(value)) {
        const guint size = gst_value_list_get_size(value);
        for (guint i = 0; i < size; ++i) {
            const GValue* item = gst_value_list_get_value(value, i);
            if (G_VALUE_HOLDS_INT(item)) {
                int val = g_value_get_int(item);
                if (val > maxInt)
                    maxInt = val;
            }
        }
    }

    return (maxInt > -G_MAXINT) ? std::make_optional<>(maxInt) : std::nullopt;
}

static std::optional<double> getMaxFractionValueFromStructure(const GstStructure* structure, ASCIILiteral fieldName)
{
    const GValue* value = gst_structure_get_value(structure, fieldName.characters());
    if (!value)
        return std::nullopt;

    double maxFraction = -G_MAXDOUBLE;
    if (GST_VALUE_HOLDS_FRACTION(value)) {
        gst_util_fraction_to_double(gst_value_get_fraction_numerator(value),
            gst_value_get_fraction_denominator(value), &maxFraction);
    } else if (GST_VALUE_HOLDS_FRACTION_RANGE(value)) {
        const GValue* fractionValue = gst_value_get_fraction_range_max(value);
        gst_util_fraction_to_double(gst_value_get_fraction_numerator(fractionValue),
            gst_value_get_fraction_denominator(fractionValue), &maxFraction);
    } else if (GST_VALUE_HOLDS_ARRAY(value)) {
        const guint size = gst_value_array_get_size(value);
        for (guint i = 0; i < size; ++i) {
            const GValue* item = gst_value_array_get_value(value, i);
            if (GST_VALUE_HOLDS_FRACTION(item)) {
                double val = -G_MAXDOUBLE;
                gst_util_fraction_to_double(gst_value_get_fraction_numerator(item),
                    gst_value_get_fraction_denominator(item), &val);
                if (val > maxFraction)
                    maxFraction = val;
            }
        }
    } else if (GST_VALUE_HOLDS_LIST(value)) {
        const guint size = gst_value_list_get_size(value);
        for (guint i = 0; i < size; ++i) {
            const GValue* item = gst_value_list_get_value(value, i);
            if (GST_VALUE_HOLDS_FRACTION(item)) {
                double val = -G_MAXDOUBLE;
                gst_util_fraction_to_double(gst_value_get_fraction_numerator(item),
                    gst_value_get_fraction_denominator(item), &val);
                if (val > maxFraction)
                    maxFraction = val;
            }
        }
    }

    return (maxFraction > -G_MAXDOUBLE) ? std::make_optional<>(maxFraction) : std::nullopt;
}

struct CaptureDeviceMode {
    String mimeType;
    String format;
    int width { 0 };
    int height { 0 };
    double frameRate { 0 };
};

static std::optional<CaptureDeviceMode> bestCaptureDeviceMode(const GstCaps* deviceCaps, int requestedWidth, int requestedHeight, double requestedFrameRate)
{
    static constexpr int maximumPreferredUncompressedWidth = 640;
    static constexpr int maximumPreferredUncompressedHeight = 480;
    bool prefersCompressed = requestedWidth > maximumPreferredUncompressedWidth || requestedHeight > maximumPreferredUncompressedHeight;
    double requestedAspectRatio = static_cast<double>(requestedWidth) / requestedHeight;

    auto rank = [&](const CaptureDeviceMode& mode) {
        bool covers = mode.width >= requestedWidth && mode.height >= requestedHeight;
        bool reachesFrameRate = mode.frameRate + 0.5 >= requestedFrameRate;
        bool matchesAspectRatio = std::abs(static_cast<double>(mode.width) / mode.height - requestedAspectRatio) <= requestedAspectRatio * 0.02;
        int64_t area = static_cast<int64_t>(mode.width) * mode.height;
        bool isCompressed = mode.mimeType == "image/jpeg"_s;
        return std::tuple { !covers, !reachesFrameRate, !matchesAspectRatio, covers ? area : -area, isCompressed != prefersCompressed, -mode.frameRate };
    };

    std::optional<CaptureDeviceMode> best;
    for (unsigned index = 0; index < gst_caps_get_size(deviceCaps); ++index) {
        auto* features = gst_caps_get_features(deviceCaps, index);
        if (features && !gst_caps_features_is_any(features) && !gst_caps_features_is_equal(features, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
            continue;

        auto* structure = gst_caps_get_structure(deviceCaps, index);
        CaptureDeviceMode mode;
        if (gst_structure_has_name(structure, "video/x-raw")) {
            mode.mimeType = "video/x-raw"_s;
            mode.format = gstStructureGetString(structure, "format"_s).span();
        } else if (gst_structure_has_name(structure, "image/jpeg"))
            mode.mimeType = "image/jpeg"_s;
        else
            continue;

        auto width = getMaxIntValueFromStructure(structure, "width"_s);
        auto height = getMaxIntValueFromStructure(structure, "height"_s);
        if (!width || !height || *width <= 0 || *height <= 0)
            continue;

        mode.width = *width;
        mode.height = *height;
        mode.frameRate = getMaxFractionValueFromStructure(structure, "framerate"_s).value_or(0);
        if (!best || rank(mode) < rank(*best))
            best = WTF::move(mode);
    }
    return best;
}

void GStreamerVideoCapturer::reconfigure()
{
    if (isCapturingDisplay()) {
        // Pipewiresrc doesn't seem to support caps re-negotiation and framerate configuration properly.
        GST_FIXME_OBJECT(m_pipeline.get(), "Caps re-negotiation disabled on display capture source");
        return;
    }

    if (!m_videoSrcMIMETypeFilter)
        return;

    auto deviceCaps = caps();
    if (!deviceCaps)
        return;

    const GstStructure* capsStruct = gst_caps_get_structure(m_caps.get(), 0);
    int requestedWidth = gstStructureGet<int>(capsStruct, "width"_s).value_or(1920);
    int requestedHeight = gstStructureGet<int>(capsStruct, "height"_s).value_or(1080);
    double requestedFrameRate = 24;
    int numerator = 0;
    int denominator = 1;
    if (gst_structure_get_fraction(capsStruct, "framerate", &numerator, &denominator) && numerator > 0)
        gst_util_fraction_to_double(numerator, denominator, &requestedFrameRate);

    if (requestedWidth <= 0 || requestedHeight <= 0)
        return;

    if (m_aspectRatioCrop)
        g_object_set(m_aspectRatioCrop.get(), "aspect-ratio", requestedWidth, requestedHeight, nullptr);

    GST_DEBUG_OBJECT(m_pipeline.get(), "Searching best video capture device mode for resolution %dx%d@%.3f", requestedWidth, requestedHeight, requestedFrameRate);

    auto mode = bestCaptureDeviceMode(deviceCaps.get(), requestedWidth, requestedHeight, requestedFrameRate);
    if (!mode) {
        GST_WARNING_OBJECT(m_pipeline.get(), "No usable video capture device mode in %" GST_PTR_FORMAT, deviceCaps.get());
        return;
    }

    GRefPtr caps = adoptGRef(gst_caps_new_simple(mode->mimeType.ascii().data(), "width", G_TYPE_INT, mode->width, "height", G_TYPE_INT, mode->height, nullptr));
    if (!mode->format.isEmpty())
        gst_caps_set_simple(caps.get(), "format", G_TYPE_STRING, mode->format.ascii().data(), nullptr);

    GST_INFO_OBJECT(m_pipeline.get(), "Setting video capture device caps to %" GST_PTR_FORMAT, caps.get());
    g_object_set(m_videoSrcMIMETypeFilter.get(), "caps", caps.get(), nullptr);
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
