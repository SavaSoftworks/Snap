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

#pragma once

#if ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(GBM)
#include "DMABufBuffer.h"
#include "GraphicsContextGLTextureMapperANGLE.h"
#include "GraphicsLayerContentsDisplayDelegate.h"
#include <wtf/unix/UnixFileDescriptor.h>

typedef void* EGLImageKHR;
struct gbm_bo;

namespace WebCore {
class DMABufBuffer;

class GraphicsContextGLTextureMapperGBM final : public GraphicsContextGLTextureMapperANGLE {
public:
    static RefPtr<GraphicsContextGLTextureMapperGBM> create(GraphicsContextGLAttributes&&, RefPtr<GraphicsLayerContentsDisplayDelegate>&& = nullptr);
    virtual ~GraphicsContextGLTextureMapperGBM();

    static bool checkRequirements();

    WTF::UnixFileDescriptor createExportedFence() const;
    void prepareForDisplayWithFinishedSignal(Function<void()>&&);
    DMABufBuffer* displayBuffer() { return m_displayBuffer.dmabuf.get(); }
#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)
    struct VideoFrameBufferCopy {
        RefPtr<DMABufBuffer> buffer;
        bool isNewBuffer { false };
        Vector<uint64_t> retiredBufferIDs;
    };
    VideoFrameBufferCopy copySurfaceBufferToVideoFrameBuffer(SurfaceBuffer, std::span<const uint64_t> releasedBufferIDs, std::span<const uint64_t> preferredModifiers);
#if USE(GSTREAMER)
    RefPtr<VideoFrame> surfaceBufferToVideoFrame(SurfaceBuffer) final;
#endif
#endif

#if ENABLE(WEBXR)
    GCGLExternalImage createExternalImage(ExternalImageSource&&, GCGLenum internalFormat, GCGLint layer) final;
    void bindExternalImage(GCGLenum target, GCGLExternalImage) final;
    bool enableRequiredWebXRExtensions() final;
#endif

private:
    GraphicsContextGLTextureMapperGBM(GraphicsContextGLAttributes&&, RefPtr<GraphicsLayerContentsDisplayDelegate>&&);

    bool platformInitialize() override;
    bool platformInitializeExtensions() override;
    bool reshapeDrawingBuffer() override;
    void prepareForDisplay() override;
#if ENABLE(WEBXR)
    bool enableRequiredWebXRExtensionsImpl();
#endif

    void freeDrawingBuffers();
    bool bindNextDrawingBuffer();

    struct DrawingBuffer {
        RefPtr<DMABufBuffer> dmabuf;
        EGLImageKHR image { nullptr };
        GCGLuint readTexture { 0 };
    };
    DrawingBuffer createDrawingBuffer() const;

    struct {
        uint32_t fourcc { 0 };
        Vector<uint64_t, 1> modifiers;
    } m_drawingBufferFormat;

    DrawingBuffer m_drawingBuffer;
    DrawingBuffer m_displayBuffer;
#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)
    struct VideoFrameBuffer {
        RefPtr<DMABufBuffer> dmabuf;
        EGLImageKHR image { nullptr };
        GCGLuint texture { 0 };
        bool isInUse { false };
        bool wasDelivered { false };
    };
    VideoFrameBuffer createVideoFrameBuffer(const IntSize&, uint32_t fourcc, std::span<const uint64_t> modifiers);
    void destroyVideoFrameBuffer(VideoFrameBuffer&);
    void freeVideoFrameBuffers();
    std::optional<size_t> acquireVideoFrameBuffer(const IntSize&, uint32_t fourcc, std::span<const uint64_t> preferredModifiers, Vector<uint64_t>& retiredBufferIDs);
    GCGLuint videoFrameSourceTexture(SurfaceBuffer);
    void publishVideoFrameBufferContents(const VideoFrameBuffer&);

    Vector<VideoFrameBuffer> m_videoFrameBuffers;
    bool m_canCopyVideoFrames { false };
    bool m_hasReportedVideoFramePoolExhaustion { false };
    bool m_videoFrameModifiersAreUnusable { false };
#if USE(GSTREAMER)
    RefPtr<DMABufBufferReleaseQueue> m_videoFrameReleaseQueue;
#endif
#endif
};

} // namespace WebCore

#endif // ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(GBM)
