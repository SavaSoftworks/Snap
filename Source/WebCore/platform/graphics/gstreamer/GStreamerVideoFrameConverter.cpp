/*
 * Copyright (C) 2025 Igalia S.L
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
#include "GStreamerVideoFrameConverter.h"

#if ENABLE(VIDEO) && USE(GSTREAMER)

#include "FloatSize.h"
#include "GStreamerCommon.h"
#include "GUniquePtrGStreamer.h"
#include <algorithm>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/glib/RunLoopSourcePriority.h>

#if USE(GSTREAMER_GL)
#include <gst/gl/gl.h>
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerVideoFrameConverter);
WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerVideoFrameConverter::Pipeline);
#if USE(GSTREAMER_GL)
WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerVideoFrameConverter::GPUPipeline);
#endif

GST_DEBUG_CATEGORY_STATIC(webkit_gst_video_frame_converter_debug);
#define GST_CAT_DEFAULT webkit_gst_video_frame_converter_debug

static const Seconds s_releaseUnusedPipelinesTimerInterval = 30_s;

GStreamerVideoFrameConverter::Pipeline::Pipeline()
    : m_src(makeGStreamerElement("appsrc"_s))
    , m_sink(makeGStreamerElement("appsink"_s))
{
    g_object_set(m_sink.get(), "enable-last-sample", FALSE, "max-buffers", 1, nullptr);
    auto videoConvert = createVideoConvertScaleElement();
    RELEASE_ASSERT(videoConvert);
    m_pipeline = gst_element_factory_make("pipeline", "video-frame-converter");
    gst_bin_add_many(GST_BIN_CAST(m_pipeline.get()), m_src.get(), videoConvert.get(), m_sink.get(), nullptr);
    gst_element_link_many(m_src.get(), videoConvert.get(), m_sink.get(), nullptr);
}

GStreamerVideoFrameConverter::Pipeline::~Pipeline() = default;

GRefPtr<GstSample> GStreamerVideoFrameConverter::Pipeline::run(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)
{
    unsigned capsSize = gst_caps_get_size(destinationCaps);
    GRefPtr newCaps = adoptGRef(gst_caps_new_empty());
    for (unsigned i = 0; i < capsSize; i++) {
        auto structure = gst_caps_get_structure(destinationCaps, i);
        auto modifiedStructure = gst_structure_copy(structure);
        gst_structure_remove_field(modifiedStructure, "framerate");
        gst_caps_append_structure(newCaps.get(), modifiedStructure);
    }

    auto destinationSize = getVideoResolutionFromCaps(newCaps.get());
    ASSERT_UNUSED(destinationSize, destinationSize && !destinationSize->isEmpty());

    GST_TRACE_OBJECT(m_pipeline.get(), "Converting sample with caps %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, gst_sample_get_caps(sample.get()), newCaps.get());
    g_object_set(m_sink.get(), "caps", newCaps.get(), nullptr);

    auto scopeExit = WTF::makeScopeExit([&] {
        gst_element_set_state(m_pipeline.get(), GST_STATE_NULL);
    });

    gst_element_set_state(m_pipeline.get(), GST_STATE_PAUSED);
    gst_app_src_push_sample(GST_APP_SRC_CAST(m_src.get()), sample.get());

    GRefPtr bus = adoptGRef(gst_element_get_bus(m_pipeline.get()));
    GRefPtr message = adoptGRef(gst_bus_timed_pop_filtered(bus.get(), 400 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_ASYNC_DONE)));
    if (!message) {
        GST_ERROR_OBJECT(m_pipeline.get(), "Video frame conversion 400ms timeout expired.");
        return nullptr;
    }

    if (GST_MESSAGE_TYPE(message.get()) == GST_MESSAGE_ERROR) {
        GST_ERROR_OBJECT(m_pipeline.get(), "Unable to convert video frame. Error: %" GST_PTR_FORMAT, message.get());
        return nullptr;
    }

    return adoptGRef(gst_app_sink_pull_preroll(GST_APP_SINK_CAST(m_sink.get())));
}

GStreamerVideoFrameConverter& GStreamerVideoFrameConverter::singleton()
{
    static NeverDestroyed<GStreamerVideoFrameConverter> sharedInstance;
    return sharedInstance;
}

GStreamerVideoFrameConverter::GStreamerVideoFrameConverter()
    : m_releaseUnusedPipelinesTimer(RunLoop::mainSingleton(), "GStreamerVideoFrameConverter::ReleaseUnusedPipelinesTimer"_s, this, &GStreamerVideoFrameConverter::releaseUnusedPipelinesTimerFired)
{
    ensureGStreamerInitialized();
    GST_DEBUG_CATEGORY_INIT(webkit_gst_video_frame_converter_debug, "webkitvideoframeconverter", 0, "WebKit GStreamer Video Frame Converter");
    m_releaseUnusedPipelinesTimer.setPriority(RunLoopSourcePriority::ReleaseUnusedResourcesTimer);
}

void GStreamerVideoFrameConverter::scheduleReleaseOfUnusedPipelines()
{
    if (!m_releaseUnusedPipelinesTimer.isActive())
        m_releaseUnusedPipelinesTimer.startOneShot(s_releaseUnusedPipelinesTimerInterval);
}

GStreamerVideoFrameConverter::Pipeline& GStreamerVideoFrameConverter::ensurePipeline()
{
    scheduleReleaseOfUnusedPipelines();
    if (!m_systemMemoryPipeline)
        m_systemMemoryPipeline = makeUnique<Pipeline>();
    m_systemMemoryPipelineLastUse = MonotonicTime::now();
    return *m_systemMemoryPipeline;
}

static bool isSystemMemoryRawVideo(GstCaps* caps)
{
    if (!caps || gst_caps_get_size(caps) != 1)
        return false;

    auto* features = gst_caps_get_features(caps, 0);
    if (features && !gst_caps_features_is_equal(features, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
        return false;

    auto* structure = gst_caps_get_structure(caps, 0);
    auto* format = gst_structure_get_string(structure, "format");
    return gst_structure_has_name(structure, "video/x-raw") && format && g_strcmp0(format, "DMA_DRM");
}

struct SystemMemoryVideoConverter {
    GstVideoInfo inputInfo;
    GstVideoInfo outputInfo;
    GUniquePtr<GstVideoConverter> converter;
};

static GRefPtr<GstSample> convertInSystemMemory(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)
{
    auto* inputCaps = gst_sample_get_caps(sample.get());
    auto* inputBuffer = gst_sample_get_buffer(sample.get());
    if (!inputBuffer || !isSystemMemoryRawVideo(inputCaps) || !isSystemMemoryRawVideo(destinationCaps))
        return nullptr;

    GstVideoInfo inputInfo;
    GstVideoInfo outputInfo;
    if (!gst_video_info_from_caps(&inputInfo, inputCaps) || !gst_video_info_from_caps(&outputInfo, destinationCaps))
        return nullptr;

    static thread_local SystemMemoryVideoConverter cache;
    if (!cache.converter || !gst_video_info_is_equal(&cache.inputInfo, &inputInfo) || !gst_video_info_is_equal(&cache.outputInfo, &outputInfo)) {
        cache.converter.reset(gst_video_converter_new(&inputInfo, &outputInfo, nullptr));
        cache.inputInfo = inputInfo;
        cache.outputInfo = outputInfo;
    }
    if (!cache.converter)
        return nullptr;

    GstVideoFrame inputFrame;
    if (!gst_video_frame_map(&inputFrame, &inputInfo, inputBuffer, GST_MAP_READ))
        return nullptr;

    GRefPtr outputBuffer = adoptGRef(gst_buffer_new_allocate(nullptr, GST_VIDEO_INFO_SIZE(&outputInfo), nullptr));
    GstVideoFrame outputFrame;
    bool mapped = outputBuffer && gst_video_frame_map(&outputFrame, &outputInfo, outputBuffer.get(), GST_MAP_WRITE);
    if (mapped) {
        gst_video_converter_frame(cache.converter.get(), &inputFrame, &outputFrame);
        gst_video_frame_unmap(&outputFrame);
    }
    gst_video_frame_unmap(&inputFrame);
    if (!mapped)
        return nullptr;

    gst_buffer_copy_into(outputBuffer.get(), inputBuffer, GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
    return adoptGRef(gst_sample_new(outputBuffer.get(), destinationCaps, gst_sample_get_segment(sample.get()), nullptr));
}

static GRefPtr<GstSample> convertMappedInSystemMemory(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)
{
    auto* caps = gst_sample_get_caps(sample.get());
    auto* buffer = gst_sample_get_buffer(sample.get());
    if (!caps || !buffer)
        return nullptr;

    GstVideoInfo info;
#if GST_CHECK_VERSION(1, 24, 0)
    if (gst_video_is_dma_drm_caps(caps)) {
        GstVideoInfoDmaDrm drmInfo;
        if (!gst_video_info_dma_drm_from_caps(&drmInfo, caps) || drmInfo.drm_modifier || !gst_video_info_dma_drm_to_video_info(&drmInfo, &info))
            return nullptr;
    } else
#endif
        if (!gst_video_info_from_caps(&info, caps))
        return nullptr;

    GRefPtr systemMemoryCaps = adoptGRef(gst_video_info_to_caps(&info));
    GRefPtr systemMemorySample = adoptGRef(gst_sample_new(buffer, systemMemoryCaps.get(), gst_sample_get_segment(sample.get()), nullptr));
    return convertInSystemMemory(systemMemorySample, destinationCaps);
}

#if USE(GSTREAMER_GL)
static constexpr size_t maximumGPUPipelines = 4;
static constexpr Seconds gpuPipelineRetryDelay = 5_s;

static bool isGPUMemory(GstCaps* caps)
{
    if (!caps || !gst_caps_get_size(caps))
        return false;
    auto* features = gst_caps_get_features(caps, 0);
    return features && (gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF) || gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY));
}

static GRefPtr<GstCaps> capsWithoutFrameRate(GstCaps* caps)
{
    GRefPtr result = adoptGRef(gst_caps_copy(caps));
    for (unsigned index = 0; index < gst_caps_get_size(result.get()); ++index)
        gst_structure_remove_fields(gst_caps_get_structure(result.get(), index), "framerate", "max-framerate", nullptr);
    return result;
}

static bool isGLColorConvertOutputFormat(const char* format)
{
    static constexpr std::array<ASCIILiteral, 12> formats { "I420"_s, "YV12"_s, "NV12"_s, "NV21"_s, "Y444"_s, "Y42B"_s, "A420"_s, "AYUV"_s, "RGBA"_s, "BGRA"_s, "RGBx"_s, "BGRx"_s };
    if (!format)
        return false;
    auto name = CStringView::unsafeFromUTF8(format);
    return std::ranges::any_of(formats, [&](auto candidate) { return name == candidate; });
}

GStreamerVideoFrameConverter::GPUPipeline::GPUPipeline(GRefPtr<GstCaps>&& inputCaps, GRefPtr<GstCaps>&& outputCaps)
    : m_inputCaps(WTF::move(inputCaps))
    , m_outputCaps(WTF::move(outputCaps))
{
}

std::unique_ptr<GStreamerVideoFrameConverter::GPUPipeline> GStreamerVideoFrameConverter::GPUPipeline::create(GRefPtr<GstCaps>&& inputCaps, GRefPtr<GstCaps>&& outputCaps)
{
    std::unique_ptr<GPUPipeline> pipeline(new GPUPipeline(WTF::move(inputCaps), WTF::move(outputCaps)));
    pipeline->m_isUsable = pipeline->initialize();
    if (pipeline->m_isUsable)
        GST_INFO("Built a GPU pipeline converting %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, pipeline->m_inputCaps.get(), pipeline->m_outputCaps.get());
    else {
        GST_WARNING("Unable to build a GPU pipeline converting %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, pipeline->m_inputCaps.get(), pipeline->m_outputCaps.get());
        pipeline->m_failureTime = MonotonicTime::now();
        pipeline->tearDown();
    }
    return pipeline;
}

GStreamerVideoFrameConverter::GPUPipeline::~GPUPipeline()
{
    tearDown();
}

void GStreamerVideoFrameConverter::GPUPipeline::tearDown()
{
    m_isUsable = false;
    if (auto pad = std::exchange(m_sourcePad, nullptr))
        gst_pad_set_active(pad.get(), FALSE);
    if (auto pipeline = std::exchange(m_pipeline, nullptr))
        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    m_sink = nullptr;
}

bool GStreamerVideoFrameConverter::GPUPipeline::matches(GstCaps* inputCaps, GstCaps* outputCaps) const
{
    return gst_caps_is_equal(m_inputCaps.get(), inputCaps) && gst_caps_is_equal(m_outputCaps.get(), outputCaps);
}

bool GStreamerVideoFrameConverter::GPUPipeline::initialize()
{
    auto* outputStructure = gst_caps_get_structure(m_outputCaps.get(), 0);
    auto width = gstStructureGet<int>(outputStructure, "width"_s);
    auto height = gstStructureGet<int>(outputStructure, "height"_s);
    auto* format = gst_structure_get_string(outputStructure, "format");
    if (!width || !height || *width <= 0 || *height <= 0 || !format)
        return false;

    GRefPtr<GstElement> upload = makeGStreamerElement("glupload"_s);
    GRefPtr<GstElement> inputConvert = makeGStreamerElement("glcolorconvert"_s);
    GRefPtr<GstElement> scale = makeGStreamerElement("glcolorscale"_s);
    GRefPtr<GstElement> outputConvert = makeGStreamerElement("glcolorconvert"_s);
    GRefPtr<GstElement> glCapsFilter = makeGStreamerElement("capsfilter"_s);
    GRefPtr<GstElement> download = makeGStreamerElement("gldownload"_s);
    GRefPtr<GstElement> videoConvert = makeGStreamerElement("videoconvert"_s);
    m_sink = makeGStreamerElement("appsink"_s);
    m_pipeline = gst_pipeline_new(nullptr);
    if (!upload || !inputConvert || !scale || !outputConvert || !glCapsFilter || !download || !videoConvert || !m_sink || !m_pipeline)
        return false;

    g_object_set(m_sink.get(), "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, "max-buffers", 1, "caps", m_outputCaps.get(), nullptr);
    GRefPtr glCaps = adoptGRef(gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, isGLColorConvertOutputFormat(format) ? format : "RGBA", "width", G_TYPE_INT, *width, "height", G_TYPE_INT, *height, "texture-target", G_TYPE_STRING, "2D", nullptr));
    if (auto* colorimetry = gst_structure_get_string(outputStructure, "colorimetry"))
        gst_caps_set_simple(glCaps.get(), "colorimetry", G_TYPE_STRING, colorimetry, nullptr);
    gst_caps_set_features(glCaps.get(), 0, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_GL_MEMORY, nullptr));
    g_object_set(glCapsFilter.get(), "caps", glCaps.get(), nullptr);

    gst_bin_add_many(GST_BIN_CAST(m_pipeline.get()), upload.get(), inputConvert.get(), scale.get(), outputConvert.get(), glCapsFilter.get(), download.get(), videoConvert.get(), m_sink.get(), nullptr);
    if (!gst_element_link_many(upload.get(), inputConvert.get(), scale.get(), outputConvert.get(), glCapsFilter.get(), download.get(), videoConvert.get(), m_sink.get(), nullptr))
        return false;

    static ASCIILiteral displayContextType = ASCIILiteral::fromLiteralUnsafe(GST_GL_DISPLAY_CONTEXT_TYPE);
    if (!setGstElementGLContext(m_pipeline.get(), displayContextType) || !setGstElementGLContext(m_pipeline.get(), "gst.gl.app_context"_s))
        return false;

    GRefPtr bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE_CAST(m_pipeline.get())));
    gst_bus_set_flushing(bus.get(), TRUE);

    m_sourcePad = gst_pad_new("src", GST_PAD_SRC);
    GRefPtr uploadSinkPad = adoptGRef(gst_element_get_static_pad(upload.get(), "sink"));
    if (gst_pad_link(m_sourcePad.get(), uploadSinkPad.get()) != GST_PAD_LINK_OK || !gst_pad_set_active(m_sourcePad.get(), TRUE))
        return false;

    if (gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        return false;

    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    return gst_pad_push_event(m_sourcePad.get(), gst_event_new_stream_start("webkit-video-frame-converter"))
        && gst_pad_push_event(m_sourcePad.get(), gst_event_new_caps(m_inputCaps.get()))
        && gst_pad_push_event(m_sourcePad.get(), gst_event_new_segment(&segment));
}

GRefPtr<GstSample> GStreamerVideoFrameConverter::GPUPipeline::run(GstBuffer* buffer)
{
    if (!m_isUsable)
        return nullptr;

    auto result = gst_pad_push(m_sourcePad.get(), gst_buffer_ref(buffer));
    GRefPtr<GstSample> sample;
    if (result == GST_FLOW_OK)
        sample = adoptGRef(gst_app_sink_try_pull_sample(GST_APP_SINK_CAST(m_sink.get()), 0));
    auto* outputBuffer = sample ? gst_sample_get_buffer(sample.get()) : nullptr;
    bool downloaded = !!outputBuffer;
    for (unsigned index = 0; downloaded && index < gst_buffer_n_memory(outputBuffer); ++index) {
        auto* memory = gst_buffer_peek_memory(outputBuffer, index);
        GstMapInfo mapInfo;
        downloaded = gst_memory_map(memory, &mapInfo, GST_MAP_READ);
        if (downloaded)
            gst_memory_unmap(memory, &mapInfo);
    }
    if (downloaded)
        return sample;

    GST_WARNING("Converting %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT " on the GPU failed: %s", m_inputCaps.get(), m_outputCaps.get(), result == GST_FLOW_OK ? "the converted frame could not be read back" : gst_flow_get_name(result));
    m_failureTime = MonotonicTime::now();
    tearDown();
    return nullptr;
}

GRefPtr<GstSample> GStreamerVideoFrameConverter::convertOnGPU(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)
{
    auto* buffer = gst_sample_get_buffer(sample.get());
    auto* destinationFeatures = gst_caps_get_size(destinationCaps) ? gst_caps_get_features(destinationCaps, 0) : nullptr;
    if (!buffer || (destinationFeatures && !gst_caps_features_is_equal(destinationFeatures, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)))
        return nullptr;

    scheduleReleaseOfUnusedPipelines();
    auto inputCaps = capsWithoutFrameRate(gst_sample_get_caps(sample.get()));
    auto outputCaps = capsWithoutFrameRate(destinationCaps);
    auto now = MonotonicTime::now();
    auto index = m_gpuPipelines.findIf([&](auto& pipeline) {
        return pipeline->matches(inputCaps.get(), outputCaps.get());
    });
    if (index != notFound && !m_gpuPipelines[index]->isUsable() && now - m_gpuPipelines[index]->failureTime() >= gpuPipelineRetryDelay) {
        m_gpuPipelines.removeAt(index);
        index = notFound;
    }
    if (index == notFound) {
        if (m_gpuPipelines.size() >= maximumGPUPipelines) {
            size_t oldest = 0;
            for (size_t candidate = 1; candidate < m_gpuPipelines.size(); ++candidate) {
                if (m_gpuPipelines[candidate]->lastUse < m_gpuPipelines[oldest]->lastUse)
                    oldest = candidate;
            }
            m_gpuPipelines.removeAt(oldest);
        }
        m_gpuPipelines.append(GPUPipeline::create(WTF::move(inputCaps), WTF::move(outputCaps)));
        index = m_gpuPipelines.size() - 1;
    }

    auto& pipeline = *m_gpuPipelines[index];
    pipeline.lastUse = now;
    if (auto result = pipeline.run(buffer))
        return result;
    return convertMappedInSystemMemory(sample, destinationCaps);
}
#endif

GRefPtr<GstSample> GStreamerVideoFrameConverter::convert(const GRefPtr<GstSample>& sample, const GRefPtr<GstCaps>& destinationCaps)
{
    auto inputCaps = gst_sample_get_caps(sample.get());
    if (gst_caps_is_equal(inputCaps, destinationCaps.get()))
        return GRefPtr(sample);

    auto outputSample = convertInSystemMemory(sample, destinationCaps.get());
    if (!outputSample) {
        Locker locker { m_lock };
        auto* inputFeatures = gst_caps_get_size(inputCaps) ? gst_caps_get_features(inputCaps, 0) : nullptr;
        bool isSystemMemory = !inputFeatures || gst_caps_features_is_equal(inputFeatures, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY);
#if USE(GSTREAMER_GL)
        if (isGPUMemory(inputCaps))
            outputSample = convertOnGPU(sample, destinationCaps.get());
        else
#endif
            if (isSystemMemory)
            outputSample = ensurePipeline().run(sample, destinationCaps.get());
        else
            outputSample = convertMappedInSystemMemory(sample, destinationCaps.get());
    }
    if (!outputSample)
        return nullptr;

    GRefPtr convertedSample = adoptGRef(gst_sample_make_writable(outputSample.leakRef()));
    gst_sample_set_caps(convertedSample.get(), destinationCaps.get());

    GRefPtr buffer = gst_sample_get_buffer(convertedSample.get());
IGNORE_WARNINGS_BEGIN("cast-align")
    GRefPtr writableBuffer = adoptGRef(gst_buffer_make_writable(buffer.leakRef()));
IGNORE_WARNINGS_END

    if (auto meta = gst_buffer_get_parent_buffer_meta(writableBuffer.get()))
        gst_buffer_remove_meta(writableBuffer.get(), GST_META_CAST(meta));

    auto structure = gst_caps_get_structure(destinationCaps.get(), 0);
    auto width = gstStructureGet<int>(structure, "width"_s);
    RELEASE_ASSERT(width && *width);
    auto height = gstStructureGet<int>(structure, "height"_s);
    RELEASE_ASSERT(height && *height);
    auto formatString = gstStructureGetString(structure, "format"_s);
    RELEASE_ASSERT(!formatString.isEmpty());
    auto format = gst_video_format_from_string(formatString.utf8());
    auto* videoMeta = gst_buffer_get_video_meta(writableBuffer.get());
    if (!videoMeta || videoMeta->format != format || videoMeta->width != static_cast<guint>(*width) || videoMeta->height != static_cast<guint>(*height)) {
        if (videoMeta)
            gst_buffer_remove_meta(writableBuffer.get(), GST_META_CAST(videoMeta));
        gst_buffer_add_video_meta(writableBuffer.get(), GST_VIDEO_FRAME_FLAG_NONE, format, *width, *height);
    }
    gst_sample_set_buffer(convertedSample.get(), writableBuffer.get());

    return convertedSample;
}

void GStreamerVideoFrameConverter::releaseUnusedPipelinesTimerFired()
{
    Locker locker { m_lock };
    auto idleSince = MonotonicTime::now() - s_releaseUnusedPipelinesTimerInterval;
    if (m_systemMemoryPipeline && m_systemMemoryPipelineLastUse <= idleSince)
        m_systemMemoryPipeline = nullptr;
    bool hasPipelines = !!m_systemMemoryPipeline;
#if USE(GSTREAMER_GL)
    m_gpuPipelines.removeAllMatching([&](auto& pipeline) {
        return pipeline->lastUse <= idleSince;
    });
    hasPipelines |= !m_gpuPipelines.isEmpty();
#endif
    if (hasPipelines)
        m_releaseUnusedPipelinesTimer.startOneShot(s_releaseUnusedPipelinesTimerInterval);
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER)
