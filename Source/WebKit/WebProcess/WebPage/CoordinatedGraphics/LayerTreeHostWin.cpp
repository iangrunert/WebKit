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
#include "LayerTreeHostWin.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedSceneState.h"
#include "DrawingArea.h"
#include "WebPageInlines.h"
#include "WebPageProxyMessages.h"
#include "WebProcess.h"
#include <WebCore/AsyncScrollingCoordinator.h>
#include <WebCore/Chrome.h>
#include <WebCore/GraphicsLayerCoordinated.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PageOverlayController.h>
#include <WebCore/RenderLayerBacking.h>
#include <WebCore/RenderView.h>
#include <WebCore/ScrollingThread.h>
#include <WebCore/Settings.h>
#include <WebCore/SkiaPaintingEngine.h>
#include <WebCore/ThreadedScrollingTree.h>
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(LayerTreeHost);

LayerTreeHost::LayerTreeHost(WebPage& webPage, WebCore::PlatformDisplayID displayID)
    : m_webPage(webPage)
    , m_sceneState(CoordinatedSceneState::create())
    , m_layerFlushTimer(RunLoop::mainSingleton(), "LayerTreeHost::LayerFlushTimer"_s, this, &LayerTreeHost::layerFlushTimerFired)
    , m_displayID(displayID)
    , m_skiaPaintingEngine(SkiaPaintingEngine::create())
{
    {
        auto& rootLayer = m_sceneState->rootLayer();
        Locker locker { rootLayer.lock() };
        rootLayer.setAnchorPoint(FloatPoint3D(0, 0, 0));
        rootLayer.setSize(m_webPage.size());
    }

    scheduleRenderingUpdate();

    m_compositor = ThreadedCompositor::create(*this, *this, displayID);
    m_layerTreeContext.contextID = m_compositor->surfaceID();
}

LayerTreeHost::~LayerTreeHost()
{
    if (m_forceRepaintAsync.callback)
        m_forceRepaintAsync.callback();

    cancelRenderingUpdate();

    m_skiaPaintingEngine = nullptr;

    m_compositor->invalidate();
    m_sceneState->invalidate();
}

void LayerTreeHost::setLayerTreeStateIsFrozen(bool isFrozen)
{
    if (m_layerTreeStateIsFrozen == isFrozen)
        return;

    m_layerTreeStateIsFrozen = isFrozen;

    if (m_layerTreeStateIsFrozen)
        cancelRenderingUpdate();
    else
        scheduleRenderingUpdate();
}

void LayerTreeHost::scheduleRenderingUpdate()
{
    WTFEmitSignpost(this, ScheduleLayerFlush, "isWaitingForRenderer %i", m_isWaitingForRenderer);

    if (m_layerTreeStateIsFrozen)
        return;

    if (m_webPage.size().isEmpty())
        return;

    if (m_isWaitingForRenderer) {
        m_scheduledWhileWaitingForRenderer = true;
        return;
    }

    if (!m_layerFlushTimer.isActive())
        m_layerFlushTimer.startOneShot(0_s);
}

void LayerTreeHost::cancelRenderingUpdate()
{
    m_layerFlushTimer.stop();
}

void LayerTreeHost::flushLayers()
{
    RELEASE_ASSERT(!m_isFlushingLayers);
    if (m_layerTreeStateIsFrozen)
        return;

    SetForScope<bool> reentrancyProtector(m_isFlushingLayers, true);

    Ref page { m_webPage };
    page->updateRendering();
    page->flushPendingEditorStateUpdate();

    if (m_overlayCompositingLayer)
        m_overlayCompositingLayer->flushCompositingState(visibleContentsRect());

    page->finalizeRenderingUpdate({ FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions });

    if (m_pendingResize) {
        m_compositor->setSize(page->size(), page->deviceScaleFactor());
        auto& rootLayer = m_sceneState->rootLayer();
        Locker locker { rootLayer.lock() };
        rootLayer.setSize(page->size());
    }

    bool didChangeSceneState = m_sceneState->flush();
    if (m_compositionRequired || m_pendingResize || m_forceFrameSync || didChangeSceneState)
        commitSceneState();

    m_compositionRequired = false;
    m_pendingResize = false;
    m_forceFrameSync = false;

    page->didUpdateRendering();

    m_imageBackingStores.removeIf([](auto& it) {
        return it.value->hasOneRef();
    });

    if (m_waitUntilPaintingComplete) {
        m_sceneState->waitUntilPaintingComplete();
        m_waitUntilPaintingComplete = false;
    }
}

void LayerTreeHost::layerFlushTimerFired()
{
    WTFBeginSignpost(this, LayerFlushTimerFired, "isWaitingForRenderer %i", m_isWaitingForRenderer);

    if (m_isSuspended) {
        WTFEndSignpost(this, LayerFlushTimerFired);
        return;
    }

    if (m_isWaitingForRenderer) {
        WTFEndSignpost(this, LayerFlushTimerFired);
        return;
    }

    if (m_forceRepaintAsync.callback)
        m_forceFrameSync = true;

    flushLayers();

    WTFEndSignpost(this, LayerFlushTimerFired);
}

void LayerTreeHost::updateRootLayer()
{
    Vector<Ref<CoordinatedPlatformLayer>> children;
    if (m_rootCompositingLayer) {
        children.append(downcast<GraphicsLayerCoordinated>(m_rootCompositingLayer)->coordinatedPlatformLayer());
        if (m_overlayCompositingLayer)
            children.append(downcast<GraphicsLayerCoordinated>(m_overlayCompositingLayer)->coordinatedPlatformLayer());
    }

    m_sceneState->setRootLayerChildren(WTF::move(children));
}

void LayerTreeHost::setRootCompositingLayer(GraphicsLayer* graphicsLayer)
{
    if (m_rootCompositingLayer == graphicsLayer)
        return;

    m_rootCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::setViewOverlayRootLayer(GraphicsLayer* graphicsLayer)
{
    if (m_overlayCompositingLayer == graphicsLayer)
        return;

    m_overlayCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::updateRenderingWithForcedRepaint()
{
    if (auto* frameView = m_webPage.localMainFrameView())
        frameView->updateLayoutAndStyleIfNeededRecursive();

    m_forceFrameSync = true;
    scheduleRenderingUpdate();

    if (!m_isWaitingForRenderer)
        flushLayers();
}

void LayerTreeHost::updateRenderingWithForcedRepaintAsync(CompletionHandler<void()>&& callback)
{
    scheduleRenderingUpdate();

    ASSERT(!m_forceRepaintAsync.callback);
    m_forceRepaintAsync.callback = WTF::move(callback);
    m_forceRepaintAsync.needsFreshFlush = m_scheduledWhileWaitingForRenderer;
}

void LayerTreeHost::sizeDidChange()
{
    m_pendingResize = true;
    if (m_isWaitingForRenderer)
        scheduleRenderingUpdate();
    else {
        cancelRenderingUpdate();
        flushLayers();
    }
}

void LayerTreeHost::pauseRendering()
{
    m_isSuspended = true;
    m_compositor->suspend();
}

void LayerTreeHost::resumeRendering()
{
    m_isSuspended = false;
    m_compositor->resume();
    scheduleRenderingUpdate();
}

GraphicsLayerFactory* LayerTreeHost::graphicsLayerFactory()
{
    return this;
}

FloatRect LayerTreeHost::visibleContentsRect() const
{
    if (auto* localMainFrameView = m_webPage.localMainFrameView())
        return FloatRect({ }, localMainFrameView->sizeForVisibleContent(ScrollableArea::VisibleContentRectIncludesScrollbars::Yes));
    return m_webPage.bounds();
}

void LayerTreeHost::backgroundColorDidChange()
{
    m_compositor->backgroundColorDidChange();
}

void LayerTreeHost::attachLayer(CoordinatedPlatformLayer& layer)
{
    m_sceneState->addLayer(layer);
}

void LayerTreeHost::detachLayer(CoordinatedPlatformLayer& layer)
{
    m_sceneState->removeLayer(layer);
}

void LayerTreeHost::notifyCompositionRequired()
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        m_compositionRequiredInScrollingThread = true;
        return;
    }
#endif
    m_compositionRequired = true;
}

bool LayerTreeHost::isCompositionRequiredOrOngoing() const
{
    return m_compositionRequired || m_forceFrameSync || m_compositor->isActive();
}

void LayerTreeHost::requestComposition(CompositionReason)
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        if (!m_compositionRequiredInScrollingThread)
            return;
        m_compositionRequiredInScrollingThread = false;
    }
#endif

    m_compositor->scheduleUpdate();
}

RunLoop* LayerTreeHost::compositingRunLoop() const
{
    return m_compositor->runLoop();
}

int LayerTreeHost::maxTextureSize() const
{
    return m_compositor->maxTextureSize();
}

Ref<CoordinatedImageBackingStore> LayerTreeHost::imageBackingStore(Ref<NativeImage>&& nativeImage)
{
    auto nativeImageID = nativeImage->uniqueID();
    auto addResult = m_imageBackingStores.ensure(nativeImageID, [&] {
        return CoordinatedImageBackingStore::create(WTF::move(nativeImage));
    });
    return addResult.iterator->value;
}

Ref<GraphicsLayer> LayerTreeHost::createGraphicsLayer(GraphicsLayer::Type layerType, GraphicsLayerClient& client)
{
    return adoptRef(*new GraphicsLayerCoordinated(layerType, client, CoordinatedPlatformLayer::create(*this)));
}

RefPtr<DisplayRefreshMonitor> LayerTreeHost::createDisplayRefreshMonitor(PlatformDisplayID displayID)
{
    ASSERT(m_displayID == displayID);
    return Ref { m_compositor->displayRefreshMonitor() };
}

void LayerTreeHost::requestDisplayRefreshMonitorUpdate()
{
    m_forceFrameSync = true;
    scheduleRenderingUpdate();
}

void LayerTreeHost::handleDisplayRefreshMonitorUpdate(bool hasBeenRescheduled)
{
    renderNextFrame(hasBeenRescheduled);
}

void LayerTreeHost::willRenderFrame()
{
    if (RefPtr drawingArea = m_webPage.drawingArea())
        drawingArea->willStartRenderingUpdateDisplay();
}

void LayerTreeHost::didRenderFrame()
{
    if (RefPtr drawingArea = m_webPage.drawingArea())
        drawingArea->didCompleteRenderingUpdateDisplay();
    if (auto fps = m_compositor->fps()) {
        if (RefPtr document = m_webPage.corePage()->localTopDocument())
            document->addConsoleMessage(MessageSource::Rendering, MessageLevel::Info, makeString("FPS: "_s, *fps));
    }
}

void LayerTreeHost::commitSceneState()
{
    m_isWaitingForRenderer = true;
    m_compositionRequestID = m_compositor->requestComposition();
    WTFEmitSignpost(this, CommitSceneState, "compositionRequestID %i", m_compositionRequestID);
}

void LayerTreeHost::renderNextFrame(bool forceRepaint)
{
    WTFBeginSignpost(this, RenderNextFrame);

    m_isWaitingForRenderer = false;
    bool scheduledWhileWaitingForRenderer = std::exchange(m_scheduledWhileWaitingForRenderer, false);

    if (m_forceRepaintAsync.callback) {
        ASSERT(!m_forceRepaintAsync.needsFreshFlush || scheduledWhileWaitingForRenderer);

        if (!m_forceRepaintAsync.needsFreshFlush)
            m_forceRepaintAsync.callback();
        m_forceRepaintAsync.needsFreshFlush = false;
    }

    if (scheduledWhileWaitingForRenderer || m_layerFlushTimer.isActive() || forceRepaint) {
        m_layerFlushTimer.stop();
        if (forceRepaint)
            m_forceFrameSync = true;
        layerFlushTimerFired();
    }

    WTFEndSignpost(this, RenderNextFrame);
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
