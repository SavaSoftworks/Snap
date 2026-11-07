/*
 * Copyright (C) 2024 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GraphicsContextGLTextureMapperGBM.h"

#if ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(GBM)
#include "ANGLEHeaders.h"
#include "CoordinatedPlatformLayerBufferDMABuf.h"
#include "DMABufBuffer.h"
#include "DRMDeviceManager.h"
#include "GBMDevice.h"
#include "GBMVersioning.h"
#include "GLFence.h"
#include "Logging.h"
#include "PlatformDisplay.h"
#include "TextureMapperFlags.h"
#include <drm_fourcc.h>
#include <wtf/unix/UnixFileDescriptor.h>
#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)
#include <algorithm>
#include <array>
#include <errno.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#if USE(GSTREAMER)
#include "VideoFrame.h"
#endif
#endif

namespace WebCore {

bool GraphicsContextGLTextureMapperGBM::checkRequirements()
{
    auto& display = PlatformDisplay::sharedDisplay();
    if (display.type() != PlatformDisplay::Type::GBM)
        return false;

    const auto& eglExtensions = display.eglExtensions();
    if (!eglExtensions.KHR_image_base || !eglExtensions.EXT_image_dma_buf_import)
        return false;

    static const char* disableGBM = getenv("WEBKIT_WEBGL_DISABLE_GBM");
    return !disableGBM || *disableGBM == '0';
}

RefPtr<GraphicsContextGLTextureMapperGBM> GraphicsContextGLTextureMapperGBM::create(GraphicsContextGLAttributes&& attributes, RefPtr<GraphicsLayerContentsDisplayDelegate>&& delegate)
{
    auto context = adoptRef(new GraphicsContextGLTextureMapperGBM(WTF::move(attributes), WTF::move(delegate)));
    if (!context->initialize())
        return nullptr;
    return context;
}

GraphicsContextGLTextureMapperGBM::GraphicsContextGLTextureMapperGBM(GraphicsContextGLAttributes&& attributes, RefPtr<GraphicsLayerContentsDisplayDelegate>&& delegate)
    : GraphicsContextGLTextureMapperANGLE(WTF::move(attributes))
{
    m_layerContentsDisplayDelegate = WTF::move(delegate);
}

GraphicsContextGLTextureMapperGBM::~GraphicsContextGLTextureMapperGBM()
{
    if (!makeContextCurrent()) {
        m_drawingBuffer.readTexture = 0;
        m_displayBuffer.readTexture = 0;
#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)
        for (auto& buffer : m_videoFrameBuffers)
            buffer.texture = 0;
#endif
    }
#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)
    freeVideoFrameBuffers();
#endif
    freeDrawingBuffers();
}

bool GraphicsContextGLTextureMapperGBM::platformInitialize()
{
    auto isOpaqueFormat = [](FourCC fourcc) -> bool {
        return fourcc != DRM_FORMAT_ARGB8888
            && fourcc != DRM_FORMAT_RGBA8888
            && fourcc != DRM_FORMAT_ABGR8888
            && fourcc != DRM_FORMAT_BGRA8888
            && fourcc != DRM_FORMAT_ARGB2101010
            && fourcc != DRM_FORMAT_ABGR2101010
            && fourcc != DRM_FORMAT_ARGB16161616F
            && fourcc != DRM_FORMAT_ABGR16161616F;
    };

    bool isOpaque = !contextAttributes().alpha;
    const auto& supportedFormats = PlatformDisplay::sharedDisplay().bufferFormats();
    for (const auto& format : supportedFormats) {
        bool matchesOpacity = isOpaqueFormat(format.fourcc) == isOpaque;
        if (!matchesOpacity && m_drawingBufferFormat.fourcc)
            continue;

        m_drawingBufferFormat.fourcc = format.fourcc.value;
        m_drawingBufferFormat.modifiers = format.modifiers;
        if (matchesOpacity)
            break;
    }

    if (!m_drawingBufferFormat.fourcc)
        return false;

    return true;
}

bool GraphicsContextGLTextureMapperGBM::platformInitializeExtensions()
{
    if (!enableExtensionsImpl({ "GL_OES_EGL_image"_s }))
        return false;
#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)
    m_canCopyVideoFrames = supportsExtensionImpl("GL_CHROMIUM_copy_texture"_s);
#endif

#if ENABLE(WEBXR)
    if (contextAttributes().xrCompatible && !enableRequiredWebXRExtensionsImpl())
        return false;
#endif

    const auto& eglExtensions = PlatformDisplay::sharedDisplay().eglExtensions();
    return eglExtensions.KHR_image_base && eglExtensions.EXT_image_dma_buf_import;
}

GraphicsContextGLTextureMapperGBM::DrawingBuffer GraphicsContextGLTextureMapperGBM::createDrawingBuffer() const
{
    auto gbmDevice = DRMDeviceManager::singleton().mainGBMDevice(DRMDeviceManager::NodeType::Render);
    if (!gbmDevice)
        return { };

    const auto size = getInternalFramebufferSize();
    struct gbm_bo* bo = nullptr;
    auto enableModifiers = m_drawingBufferFormat.modifiers.size() == 1 && m_drawingBufferFormat.modifiers[0] == DRM_FORMAT_MOD_INVALID
        ? DMABufBufferAttributes::EnableModifiers::No : DMABufBufferAttributes::EnableModifiers::Yes;
    if (enableModifiers == DMABufBufferAttributes::EnableModifiers::Yes && !m_drawingBufferFormat.modifiers.isEmpty())
        bo = gbm_bo_create_with_modifiers2(gbmDevice->device(), size.width(), size.height(), m_drawingBufferFormat.fourcc, m_drawingBufferFormat.modifiers.span().data(), m_drawingBufferFormat.modifiers.size(), GBM_BO_USE_RENDERING);
    if (!bo)
        bo = gbm_bo_create(gbmDevice->device(), size.width(), size.height(), m_drawingBufferFormat.fourcc, GBM_BO_USE_RENDERING);
    if (!bo)
        return { };

    auto dmaBufAttributes = DMABufBufferAttributes::fromGBMBufferObject(bo, enableModifiers);
    if (!dmaBufAttributes) {
        gbm_bo_destroy(bo);
        return { };
    }

    Ref dmaBuf = DMABufBuffer::create(WTF::move(*dmaBufAttributes));
    auto eglAttributes = DMABufBuffer::buildEGLImageAttributes(dmaBuf->attributes());
    gbm_bo_destroy(bo);

    if (!eglAttributes)
        return { };

    auto image = EGL_CreateImageKHR(m_displayObj, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, eglAttributes->span().data());
    if (!image)
        return { };

    return { WTF::move(dmaBuf), image };
}

void GraphicsContextGLTextureMapperGBM::freeDrawingBuffers()
{
    auto destroyBuffer = [this](DrawingBuffer& buffer) {
        if (buffer.readTexture) {
            GL_DeleteTextures(1, &buffer.readTexture);
            buffer.readTexture = 0;
        }
        if (!buffer.image)
            return;

        EGL_DestroyImageKHR(m_displayObj, buffer.image);
        buffer.image = nullptr;
        buffer.dmabuf = nullptr;
    };
    destroyBuffer(m_drawingBuffer);
    destroyBuffer(m_displayBuffer);
}

bool GraphicsContextGLTextureMapperGBM::bindNextDrawingBuffer()
{
    std::swap(m_drawingBuffer, m_displayBuffer);

    if (!m_drawingBuffer.dmabuf) {
        auto buffer = createDrawingBuffer();
        if (!buffer.dmabuf)
            return false;

        m_drawingBuffer = WTF::move(buffer);
    }

    ScopedRestoreTextureBinding restoreBinding(TEXTURE_BINDING_2D, TEXTURE_2D);
    GL_BindTexture(GL_TEXTURE_2D, m_texture);
    GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_drawingBuffer.image);
    return true;
}

bool GraphicsContextGLTextureMapperGBM::reshapeDrawingBuffer()
{
    freeDrawingBuffers();
    return bindNextDrawingBuffer();
}

UnixFileDescriptor GraphicsContextGLTextureMapperGBM::createExportedFence() const
{
    const auto& eglExtensions = PlatformDisplay::sharedDisplay().eglExtensions();
    if (!eglExtensions.KHR_fence_sync || !eglExtensions.ANDROID_native_fence_sync)
        return { };

    auto sync = EGL_CreateSyncKHR(m_displayObj, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC)
        return { };

    GL_Flush();
    auto fd = UnixFileDescriptor { EGL_DupNativeFenceFDANDROID(m_displayObj, sync), UnixFileDescriptor::Adopt };
    EGL_DestroySyncKHR(m_displayObj, sync);
    return fd;
}

void GraphicsContextGLTextureMapperGBM::prepareForDisplay()
{
    UnixFileDescriptor fenceFD;
    std::unique_ptr<GLFence> fence;
    prepareForDisplayWithFinishedSignal([this, &fenceFD, &fence] {
        fenceFD = createExportedFence();
        if (!fenceFD) {
            GL_Flush();
            fence = GLFence::create(PlatformDisplay::sharedDisplay().glDisplay());
        }
    });

    if (!m_displayBuffer.dmabuf)
        return;

    RELEASE_ASSERT(m_layerContentsDisplayDelegate);
    OptionSet<TextureMapperFlags> flags = TextureMapperFlags::ShouldFlipTexture;
    if (contextAttributes().alpha)
        flags.add(TextureMapperFlags::ShouldBlend);
    std::unique_ptr<CoordinatedPlatformLayerBuffer> buffer;
    if (fenceFD)
        buffer = CoordinatedPlatformLayerBufferDMABuf::create(protect(*m_displayBuffer.dmabuf), flags, WTF::move(fenceFD));
    else
        buffer = CoordinatedPlatformLayerBufferDMABuf::create(protect(*m_displayBuffer.dmabuf), flags, WTF::move(fence));
    m_layerContentsDisplayDelegate->setDisplayBuffer(WTF::move(buffer));
}

void GraphicsContextGLTextureMapperGBM::prepareForDisplayWithFinishedSignal(Function<void()>&& finishedSignalCreator)
{
    if (!makeContextCurrent())
        return;

    if (!m_drawingBuffer.dmabuf)
        return;

    prepareTexture();
    finishedSignalCreator();

    if (!bindNextDrawingBuffer()) {
        forceContextLost();
        return;
    }
}

#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)
static constexpr size_t maximumVideoFrameBuffers = 6;

GraphicsContextGLTextureMapperGBM::VideoFrameBuffer GraphicsContextGLTextureMapperGBM::createVideoFrameBuffer(const IntSize& size, uint32_t fourcc, std::span<const uint64_t> modifiers)
{
    auto gbmDevice = DRMDeviceManager::singleton().mainGBMDevice(DRMDeviceManager::NodeType::Render);
    if (!gbmDevice || modifiers.empty())
        return {};

    bool isLinear = modifiers.size() == 1 && modifiers[0] == DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo* bo = gbm_bo_create_with_modifiers2(gbmDevice->device(), size.width(), size.height(), fourcc, modifiers.data(), modifiers.size(), GBM_BO_USE_RENDERING);
    if (!bo && isLinear)
        bo = gbm_bo_create(gbmDevice->device(), size.width(), size.height(), fourcc, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo)
        return {};

    auto dmaBufAttributes = DMABufBufferAttributes::fromGBMBufferObject(bo);
    gbm_bo_destroy(bo);
    if (!dmaBufAttributes || dmaBufAttributes->fds.size() != 1)
        return {};
    if (isLinear)
        dmaBufAttributes->modifier = DRM_FORMAT_MOD_LINEAR;
    else if (std::ranges::find(modifiers, dmaBufAttributes->modifier) == modifiers.end())
        return {};

    Ref dmabuf = DMABufBuffer::create(WTF::move(*dmaBufAttributes));
    auto enableModifiers = PlatformDisplay::sharedDisplay().eglExtensions().EXT_image_dma_buf_import_modifiers
        ? DMABufBufferAttributes::EnableModifiers::Yes
        : DMABufBufferAttributes::EnableModifiers::No;
    auto eglAttributes = DMABufBuffer::buildEGLImageAttributes(dmabuf->attributes(), enableModifiers);
    if (!eglAttributes)
        return {};

    auto image = EGL_CreateImageKHR(m_displayObj, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, eglAttributes->span().data());
    if (!image)
        return {};

    GCGLuint texture = 0;
    {
        ScopedRestoreTextureBinding restoreBinding(TEXTURE_BINDING_2D, TEXTURE_2D);
        GL_GenTextures(1, &texture);
        GL_BindTexture(GL_TEXTURE_2D, texture);
        GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    }
    return { WTF::move(dmabuf), image, texture };
}

void GraphicsContextGLTextureMapperGBM::destroyVideoFrameBuffer(VideoFrameBuffer& buffer)
{
    if (buffer.texture)
        GL_DeleteTextures(1, &buffer.texture);
    if (buffer.image)
        EGL_DestroyImageKHR(m_displayObj, buffer.image);
    buffer = {};
}

void GraphicsContextGLTextureMapperGBM::freeVideoFrameBuffers()
{
    for (auto& buffer : m_videoFrameBuffers)
        destroyVideoFrameBuffer(buffer);
    m_videoFrameBuffers.clear();
}

std::optional<size_t> GraphicsContextGLTextureMapperGBM::acquireVideoFrameBuffer(const IntSize& size, uint32_t fourcc, std::span<const uint64_t> preferredModifiers, Vector<uint64_t>& retiredBufferIDs)
{
    for (size_t index = m_videoFrameBuffers.size(); index--;) {
        auto& buffer = m_videoFrameBuffers[index];
        const auto& attributes = buffer.dmabuf->attributes();
        if (attributes.size == size && attributes.fourcc.value == fourcc)
            continue;
        retiredBufferIDs.append(buffer.dmabuf->id());
        destroyVideoFrameBuffer(buffer);
        m_videoFrameBuffers.removeAt(index);
    }

    for (size_t index = 0; index < m_videoFrameBuffers.size(); ++index) {
        if (!m_videoFrameBuffers[index].isInUse)
            return index;
    }

    if (m_videoFrameBuffers.size() >= maximumVideoFrameBuffers) {
        if (!m_hasReportedVideoFramePoolExhaustion) {
            RELEASE_LOG(WebGL, "All %zu WebGL video frame buffers are held by consumers, copying frames through the CPU until one is released", maximumVideoFrameBuffers);
            m_hasReportedVideoFramePoolExhaustion = true;
        }
        return std::nullopt;
    }

    VideoFrameBuffer buffer;
    if (!preferredModifiers.empty() && !m_videoFrameModifiersAreUnusable) {
        buffer = createVideoFrameBuffer(size, fourcc, preferredModifiers);
        if (!buffer.dmabuf) {
            RELEASE_LOG(WebGL, "None of the %zu DMA-BUF layouts the video post-processor imports can hold a %dx%d WebGL video frame, using linear buffers", preferredModifiers.size(), size.width(), size.height());
            m_videoFrameModifiersAreUnusable = true;
        }
    }
    if (!buffer.dmabuf) {
        const std::array<uint64_t, 1> linearModifier { DRM_FORMAT_MOD_LINEAR };
        buffer = createVideoFrameBuffer(size, fourcc, linearModifier);
    }
    if (!buffer.dmabuf) {
        RELEASE_LOG_ERROR(WebGL, "Unable to allocate a %dx%d DMA-BUF for WebGL video frames, copying them through the CPU", size.width(), size.height());
        m_canCopyVideoFrames = false;
        return std::nullopt;
    }
    m_videoFrameBuffers.append(WTF::move(buffer));
    return m_videoFrameBuffers.size() - 1;
}

GCGLuint GraphicsContextGLTextureMapperGBM::videoFrameSourceTexture(SurfaceBuffer source)
{
    if (source == SurfaceBuffer::DisplayBuffer && m_displayBuffer.image) {
        if (!m_displayBuffer.readTexture) {
            ScopedRestoreTextureBinding restoreBinding(TEXTURE_BINDING_2D, TEXTURE_2D);
            GL_GenTextures(1, &m_displayBuffer.readTexture);
            GL_BindTexture(GL_TEXTURE_2D, m_displayBuffer.readTexture);
            GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_displayBuffer.image);
        }
        return m_displayBuffer.readTexture;
    }

    if (!m_drawingBuffer.dmabuf)
        return 0;
    if (contextAttributes().antialias)
        resolveMultisamplingIfNecessary();
    return m_preserveDrawingBufferTexture ? m_preserveDrawingBufferTexture : m_texture;
}

void GraphicsContextGLTextureMapperGBM::publishVideoFrameBufferContents(const VideoFrameBuffer& buffer)
{
    auto fence = createExportedFence();
#if defined(DMA_BUF_IOCTL_IMPORT_SYNC_FILE)
    if (fence) {
        struct dma_buf_import_sync_file request { DMA_BUF_SYNC_WRITE, fence.value() };
        int result;
        do
            result = ioctl(buffer.dmabuf->attributes().fds[0].value(), DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &request);
        while (result == -1 && (errno == EINTR || errno == EAGAIN));
        if (!result)
            return;
    }
#else
    UNUSED_PARAM(buffer);
#endif
    GL_Finish();
}

auto GraphicsContextGLTextureMapperGBM::copySurfaceBufferToVideoFrameBuffer(SurfaceBuffer source, std::span<const uint64_t> releasedBufferIDs, std::span<const uint64_t> preferredModifiers) -> VideoFrameBufferCopy
{
    VideoFrameBufferCopy copy;
    for (auto bufferID : releasedBufferIDs) {
        for (auto& buffer : m_videoFrameBuffers) {
            if (buffer.dmabuf->id() == bufferID)
                buffer.isInUse = false;
        }
    }

    if (!m_canCopyVideoFrames || !makeContextCurrent())
        return copy;

    auto size = getInternalFramebufferSize();
    if (size.isEmpty())
        return copy;

    updateErrors();
    auto sourceTexture = videoFrameSourceTexture(source);
    if (!sourceTexture)
        return copy;

    const auto& attributes = contextAttributes();
    auto index = acquireVideoFrameBuffer(size, attributes.alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888, preferredModifiers, copy.retiredBufferIDs);
    if (!index)
        return copy;

    auto& buffer = m_videoFrameBuffers[*index];
    bool shouldUnmultiply = attributes.alpha && attributes.premultipliedAlpha;
    GL_CopySubTextureCHROMIUM(sourceTexture, 0, GL_TEXTURE_2D, buffer.texture, 0, 0, 0, 0, 0, size.width(), size.height(), GL_TRUE, GL_FALSE, shouldUnmultiply ? GL_TRUE : GL_FALSE);
    if (auto error = GL_GetError(); error != GL_NO_ERROR) {
        RELEASE_LOG_ERROR(WebGL, "Copying a WebGL surface into a video frame buffer failed with GL error 0x%x, copying frames through the CPU", error);
        m_canCopyVideoFrames = false;
        return copy;
    }

    publishVideoFrameBufferContents(buffer);
    buffer.isInUse = true;
    copy.buffer = buffer.dmabuf;
    copy.isNewBuffer = !std::exchange(buffer.wasDelivered, true);
    return copy;
}

#if USE(GSTREAMER)
RefPtr<VideoFrame> GraphicsContextGLTextureMapperGBM::surfaceBufferToVideoFrame(SurfaceBuffer source)
{
    if (!m_videoFrameReleaseQueue)
        m_videoFrameReleaseQueue = DMABufBufferReleaseQueue::create();
    auto releasedBufferIDs = m_videoFrameReleaseQueue->take();
    auto preferredModifiers = VideoFrame::preferredDMABufModifiers(contextAttributes().alpha);
    auto copy = copySurfaceBufferToVideoFrameBuffer(source, releasedBufferIDs.span(), preferredModifiers.span());
    if (!copy.buffer)
        return nullptr;
    RefPtr frame = VideoFrame::createFromDMABufBuffer(*copy.buffer, *m_videoFrameReleaseQueue);
    if (!frame)
        m_videoFrameReleaseQueue->add(copy.buffer->id());
    return frame;
}
#endif
#endif

#if ENABLE(WEBXR)
GCGLExternalImage GraphicsContextGLTextureMapperGBM::createExternalImage(ExternalImageSource&& source, GCGLenum, GCGLint)
{
    GraphicsContextGLExternalImageSource imageSource = WTF::move(source);

    if (m_displayObj == EGL_NO_DISPLAY) {
        addError(GCGLErrorCode::InvalidOperation);
        LOG(XR, "invalid display %d", EGL_GetError());
        return { };
    }

    DMABufBufferAttributes dmaBufAttributes { imageSource.size, imageSource.fourcc, WTF::move(imageSource.fds), WTF::move(imageSource.offsets), WTF::move(imageSource.strides), imageSource.modifier };
    auto enableModifiers = PlatformDisplay::sharedDisplay().eglExtensions().EXT_image_dma_buf_import_modifiers
        ? DMABufBufferAttributes::EnableModifiers::Yes : DMABufBufferAttributes::EnableModifiers::No;
    auto eglAttributes = DMABufBuffer::buildEGLImageAttributes(dmaBufAttributes, enableModifiers);
    if (!eglAttributes) {
        addError(GCGLErrorCode::InvalidOperation);
        return { };
    }
    auto eglImage = EGL_CreateImageKHR(m_displayObj, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, eglAttributes->span().data());
    if (!eglImage) {
        LOG(XR, "invalid operation importing the image %d", EGL_GetError());
        addError(GCGLErrorCode::InvalidOperation);
        return { };
    }
    auto newName = ++m_nextExternalImageName;
    m_eglImages.add(newName, eglImage);
    return newName;
}

void GraphicsContextGLTextureMapperGBM::bindExternalImage(GCGLenum target, GCGLExternalImage image)
{
    if (!makeContextCurrent())
        return;
    EGLImage eglImage = EGL_NO_IMAGE_KHR;
    if (image) {
        eglImage = m_eglImages.get(image);
        if (!eglImage) {
            addError(GCGLErrorCode::InvalidOperation);
            return;
        }
    }
    if (target == RENDERBUFFER)
        GL_EGLImageTargetRenderbufferStorageOES(RENDERBUFFER, eglImage);
    else
        GL_EGLImageTargetTexture2DOES(target, eglImage);
}

bool GraphicsContextGLTextureMapperGBM::enableRequiredWebXRExtensions()
{
    if (!makeContextCurrent())
        return false;

    return enableRequiredWebXRExtensionsImpl();
}

bool GraphicsContextGLTextureMapperGBM::enableRequiredWebXRExtensionsImpl()
{
    return enableExtensionsImpl({
        "GL_OES_EGL_image"_s,
        "GL_OES_EGL_image_external"_s,
        "GL_EXT_discard_framebuffer"_s
    });
}
#endif // ENABLE(WEBXR)

} // namespace WebCore

#endif // ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(GBM)
