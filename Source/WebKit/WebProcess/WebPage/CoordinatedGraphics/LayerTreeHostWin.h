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

#pragma once

#if USE(COORDINATED_GRAPHICS)
#include "CallbackID.h"
#include "LayerTreeContext.h"
#include "ThreadedCompositorWin.h"
#include "ThreadedDisplayRefreshMonitorWin.h"
#include <WebCore/CoordinatedImageBackingStore.h>
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/FloatPoint.h>
#include <WebCore/GraphicsLayerClient.h>
#include <WebCore/GraphicsLayerFactory.h>
#include <WebCore/PlatformScreen.h>
#include <wtf/CheckedRef.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Forward.h>
#include <wtf/Lock.h>
#include <wtf/OptionSet.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {
class IntRect;
class IntSize;
class GraphicsLayer;
class GraphicsLayerFactory;
class NativeImage;
class SkiaPaintingEngine;
}

namespace WebKit {
class CoordinatedSceneState;
class WebPage;

class LayerTreeHost final : public CanMakeCheckedPtr<LayerTreeHost>, public WebCore::GraphicsLayerFactory, public WebCore::CoordinatedPlatformLayer::Client
    , public ThreadedDisplayRefreshMonitor::Client
{
    WTF_MAKE_TZONE_ALLOCATED(LayerTreeHost);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(LayerTreeHost);
public:
    LayerTreeHost(WebPage&, WebCore::PlatformDisplayID);
    ~LayerTreeHost();

    WebPage& webPage() const { return m_webPage; }
    CoordinatedSceneState& sceneState() const { return m_sceneState.get(); }

    const LayerTreeContext& layerTreeContext() const LIFETIME_BOUND { return m_layerTreeContext; }
    void setLayerTreeStateIsFrozen(bool);

    void scheduleRenderingUpdate();
    void cancelRenderingUpdate();
    void setRootCompositingLayer(WebCore::GraphicsLayer*);
    void setViewOverlayRootLayer(WebCore::GraphicsLayer*);

    void updateRenderingWithForcedRepaint();
    void updateRenderingWithForcedRepaintAsync(CompletionHandler<void()>&&);
    void sizeDidChange();

    void pauseRendering();
    void resumeRendering();

    WebCore::GraphicsLayerFactory* graphicsLayerFactory();

    void backgroundColorDidChange();

    void willRenderFrame();
    void didRenderFrame();

    RefPtr<WebCore::DisplayRefreshMonitor> createDisplayRefreshMonitor(WebCore::PlatformDisplayID);
    WebCore::PlatformDisplayID displayID() const { return m_displayID; }

private:
    void updateRootLayer();
    WebCore::FloatRect visibleContentsRect() const;
    void layerFlushTimerFired();
    void flushLayers();
    void commitSceneState();
    void renderNextFrame(bool);

    // CoordinatedPlatformLayer::Client
    WebCore::SkiaPaintingEngine& paintingEngine() const LIFETIME_BOUND override { return *m_skiaPaintingEngine.get(); }
    Ref<WebCore::CoordinatedImageBackingStore> imageBackingStore(Ref<WebCore::NativeImage>&&) override;

    void attachLayer(WebCore::CoordinatedPlatformLayer&) override;
    void detachLayer(WebCore::CoordinatedPlatformLayer&) override;
    void notifyCompositionRequired() override;
    bool isCompositionRequiredOrOngoing() const override;
    void requestComposition(WebCore::CompositionReason) override;
    RunLoop* compositingRunLoop() const override;
    int maxTextureSize() const override;
    void willPaintTile() override { };
    void didPaintTile() override { };

    // GraphicsLayerFactory
    Ref<WebCore::GraphicsLayer> createGraphicsLayer(WebCore::GraphicsLayer::Type, WebCore::GraphicsLayerClient&) override;

    // ThreadedDisplayRefreshMonitor::Client
    void requestDisplayRefreshMonitorUpdate() override;
    void handleDisplayRefreshMonitorUpdate(bool hasBeenRescheduled) override;

    WebPage& m_webPage;
    LayerTreeContext m_layerTreeContext;
    const Ref<CoordinatedSceneState> m_sceneState;
    WebCore::GraphicsLayer* m_rootCompositingLayer { nullptr };
    WebCore::GraphicsLayer* m_overlayCompositingLayer { nullptr };
    HashSet<Ref<WebCore::CoordinatedPlatformLayer>> m_layers;
    bool m_layerTreeStateIsFrozen { false };
    bool m_pendingResize { false };
    bool m_isFlushingLayers { false };
    bool m_waitUntilPaintingComplete { false };
    bool m_isSuspended { false };
    bool m_isWaitingForRenderer { false };
    bool m_scheduledWhileWaitingForRenderer { false };
    bool m_forceFrameSync { false };
    bool m_compositionRequired { false };
#if ENABLE(SCROLLING_THREAD)
    bool m_compositionRequiredInScrollingThread { false };
#endif
    RefPtr<ThreadedCompositor> m_compositor;
    struct {
        CompletionHandler<void()> callback;
        bool needsFreshFlush { false };
    } m_forceRepaintAsync;
    RunLoop::Timer m_layerFlushTimer;
    WebCore::PlatformDisplayID m_displayID;
    std::unique_ptr<WebCore::SkiaPaintingEngine> m_skiaPaintingEngine;
    HashMap<uint64_t, Ref<WebCore::CoordinatedImageBackingStore>> m_imageBackingStores;

    uint32_t m_compositionRequestID { 0 };
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
