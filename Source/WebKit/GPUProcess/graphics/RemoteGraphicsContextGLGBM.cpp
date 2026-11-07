/*
 * Copyright (C) 2022 Metrological Group B.V.
 * Copyright (C) 2022 Igalia S.L.
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
#include "RemoteGraphicsContextGL.h"

#if ENABLE(GPU_PROCESS) && ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(GBM)

#include <wtf/TZoneMalloc.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {

class RemoteGraphicsContextGLGBM final : public RemoteGraphicsContextGL {
    WTF_MAKE_TZONE_ALLOCATED(RemoteGraphicsContextGLGBM);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(RemoteGraphicsContextGLGBM);

public:
    RemoteGraphicsContextGLGBM(GPUConnectionToWebProcess&, RemoteGraphicsContextGLIdentifier, RemoteRenderingBackend&, Ref<IPC::StreamServerConnection>&&);

private:
    void platformWorkQueueInitialize(WebCore::GraphicsContextGLAttributes&&) final;
    void prepareForDisplay(CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&)>&&) final;
#if (ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)) && USE(GSTREAMER)
    void copySurfaceBufferToVideoFrameBuffer(WebCore::GraphicsContextGL::SurfaceBuffer, Vector<uint64_t>&&, Vector<uint64_t>&&, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, Vector<uint64_t>&&)>&&) final;
#endif
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteGraphicsContextGLGBM);

RemoteGraphicsContextGLGBM::RemoteGraphicsContextGLGBM(GPUConnectionToWebProcess& connection, RemoteGraphicsContextGLIdentifier identifier, RemoteRenderingBackend& renderingBackend, Ref<IPC::StreamServerConnection>&& streamConnection)
    : RemoteGraphicsContextGL(connection, identifier, renderingBackend, WTF::move(streamConnection))
{ }

void RemoteGraphicsContextGLGBM::platformWorkQueueInitialize(WebCore::GraphicsContextGLAttributes&& attributes)
{
    assertIsCurrent(workQueue());
    m_context = WebCore::GraphicsContextGLTextureMapperGBM::create(WTF::move(attributes));
}

void RemoteGraphicsContextGLGBM::prepareForDisplay(CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&)>&& completionHandler)
{
    assertIsCurrent(workQueue());

    UnixFileDescriptor fenceFD;
    m_context->prepareForDisplayWithFinishedSignal([this, &fenceFD] {
        assertIsCurrent(workQueue());
        fenceFD = m_context->createExportedFence();
        if (!fenceFD)
            m_context->flush();
    });

    auto* buffer = m_context->displayBuffer();
    if (!buffer) {
        completionHandler(0, std::nullopt, { });
        return;
    }

    completionHandler(buffer->id(), buffer->takeAttributes(), WTF::move(fenceFD));
}

#if (ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)) && USE(GSTREAMER)
void RemoteGraphicsContextGLGBM::copySurfaceBufferToVideoFrameBuffer(WebCore::GraphicsContextGL::SurfaceBuffer buffer, Vector<uint64_t>&& releasedBufferIDs, Vector<uint64_t>&& preferredModifiers, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, Vector<uint64_t>&&)>&& completionHandler)
{
    assertIsCurrent(workQueue());
    if (!m_context) {
        completionHandler(0, std::nullopt, {});
        return;
    }

    auto copy = m_context->copySurfaceBufferToVideoFrameBuffer(buffer, releasedBufferIDs.span(), preferredModifiers.span());
    if (!copy.buffer) {
        completionHandler(0, std::nullopt, WTF::move(copy.retiredBufferIDs));
        return;
    }

    std::optional<WebCore::DMABufBuffer::Attributes> attributes;
    if (copy.isNewBuffer) {
        const auto& source = copy.buffer->attributes();
        attributes = WebCore::DMABufBuffer::Attributes { source.size, source.fourcc, source.fds.map([](const auto& fd) { return fd.duplicate(); }), source.offsets, source.strides, source.modifier };
    }
    completionHandler(copy.buffer->id(), WTF::move(attributes), WTF::move(copy.retiredBufferIDs));
}
#endif

Ref<RemoteGraphicsContextGL> RemoteGraphicsContextGL::create(GPUConnectionToWebProcess& connection, WebCore::GraphicsContextGLAttributes&& attributes, RemoteGraphicsContextGLIdentifier identifier, RemoteRenderingBackend& renderingBackend, Ref<IPC::StreamServerConnection>&& streamConnection)
{
    auto instance = adoptRef(*new RemoteGraphicsContextGLGBM(connection, identifier, renderingBackend, WTF::move(streamConnection)));
    instance->initialize(WTF::move(attributes));
    return instance;
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS) && ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && USE(GBM)
