/*
 * Copyright (c) 2026 Ian Grunert <ian.grunert@gmail.com>
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
#include "RemoteGraphicsContextGLProxy.h"

// Phase 1 stub: WebGL in the GPU process under coordinated graphics on Windows
// is not yet wired through D3D shared textures. This class lets WebKit2 link;
// the layer contents display delegate is a no-op until the D3D shared-handle
// present path lands (see silly-snuggling-scone-d3d-present.md).
#if ENABLE(GPU_PROCESS) && ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && PLATFORM(WIN)
#include <WebCore/GraphicsLayerContentsDisplayDelegateCoordinated.h>

namespace WebKit {
using namespace WebCore;

class RemoteGraphicsContextGLProxyCoordinatedWin final : public RemoteGraphicsContextGLProxy {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(RemoteGraphicsContextGLProxyCoordinatedWin);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(RemoteGraphicsContextGLProxyCoordinatedWin);
public:
    virtual ~RemoteGraphicsContextGLProxyCoordinatedWin() = default;

private:
    friend class RemoteGraphicsContextGLProxy;
    explicit RemoteGraphicsContextGLProxyCoordinatedWin(const GraphicsContextGLAttributes& attributes, RemoteRenderingBackendProxy& renderingBackend)
        : RemoteGraphicsContextGLProxy(attributes, renderingBackend)
        , m_layerContentsDisplayDelegate(GraphicsLayerContentsDisplayDelegateCoordinated::create())
    {
    }

    RefPtr<GraphicsLayerContentsDisplayDelegate> layerContentsDisplayDelegate() final { return m_layerContentsDisplayDelegate.copyRef(); }
    void prepareForDisplay() final { }

    const Ref<GraphicsLayerContentsDisplayDelegate> m_layerContentsDisplayDelegate;
};

Ref<RemoteGraphicsContextGLProxy> RemoteGraphicsContextGLProxy::platformCreate(const GraphicsContextGLAttributes& attributes, RemoteRenderingBackendProxy& renderingBackend)
{
    return adoptRef(*new RemoteGraphicsContextGLProxyCoordinatedWin(attributes, renderingBackend));
}

} // namespace WebKit

#endif
