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
#include "ThreadedCompositorWin.h"

#if USE(COORDINATED_GRAPHICS)
#include "AcceleratedSurfaceWin.h"
#include "CompositingRunLoop.h"
#include "CoordinatedSceneState.h"
#include "LayerTreeHostWin.h"
#include "ThreadedDisplayRefreshMonitorWin.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/TextureMapperLayer.h>
#include <WebCore/TransformationMatrix.h>
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

static constexpr unsigned c_defaultRefreshRate = 60000;

WTF_MAKE_TZONE_ALLOCATED_IMPL(ThreadedCompositor);

Ref<ThreadedCompositor> ThreadedCompositor::create(LayerTreeHost& layerTreeHost, ThreadedDisplayRefreshMonitor::Client& displayRefreshMonitorClient, PlatformDisplayID displayID)
{
    return adoptRef(*new ThreadedCompositor(layerTreeHost, displayRefreshMonitorClient, displayID));
}

ThreadedCompositor::ThreadedCompositor(LayerTreeHost& layerTreeHost, ThreadedDisplayRefreshMonitor::Client& displayRefreshMonitorClient, PlatformDisplayID displayID)
    : m_layerTreeHost(&layerTreeHost)
    , m_surface(AcceleratedSurface::create(layerTreeHost.webPage(), [this] { frameComplete(); }))
    , m_sceneState(&m_layerTreeHost->sceneState())
    , m_flipY(m_surface->shouldPaintMirrored())
    , m_compositingRunLoop(makeUnique<CompositingRunLoop>([this] { renderLayerTree(); }))
    , m_displayRefreshMonitor(ThreadedDisplayRefreshMonitor::create(displayID, displayRefreshMonitorClient, WebCore::DisplayUpdate { 0, c_defaultRefreshRate / 1000 }))
{
    ASSERT(RunLoop::isMain());

    initializeFPSCounter();

    const auto& webPage = m_layerTreeHost->webPage();
    updateSceneAttributes(webPage.size(), webPage.deviceScaleFactor());

    m_surface->didCreateCompositingRunLoop(m_compositingRunLoop->runLoop());

    m_display.displayID = displayID;
    m_display.displayUpdate = { 0, c_defaultRefreshRate / 1000 };

    m_compositingRunLoop->performTaskSync([this, protectedThis = Ref { *this }] {
        m_display.updateTimer = makeUnique<RunLoop::Timer>(RunLoop::currentSingleton(), "ThreadedCompositor::UpdateTimer"_s, this, &ThreadedCompositor::displayUpdateFired);
        m_display.updateTimer->startOneShot(Seconds { 1.0 / m_display.displayUpdate.updatesPerSecond });

        static_assert(sizeof(GLNativeWindowType) <= sizeof(uint64_t), "GLNativeWindowType must not be longer than 64 bits.");
        auto nativeSurfaceHandle = (GLNativeWindowType)m_surface->window();
        m_context = GLContext::create(PlatformDisplay::sharedDisplay(), nativeSurfaceHandle);
        if (m_context && m_context->makeContextCurrent()) {
            if (!nativeSurfaceHandle)
                m_flipY = !m_flipY;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
        }
    });
}

ThreadedCompositor::~ThreadedCompositor() = default;

uint64_t ThreadedCompositor::surfaceID() const
{
    ASSERT(RunLoop::isMain());
    return m_surface->surfaceID();
}

void ThreadedCompositor::invalidate()
{
    ASSERT(RunLoop::isMain());
    m_compositingRunLoop->stopUpdates();
    m_displayRefreshMonitor->invalidate();
    m_compositingRunLoop->performTaskSync([this, protectedThis = Ref { *this }] {
        if (!m_context || !m_context->makeContextCurrent())
            return;

        updateSceneState();

        m_sceneState->invalidateCommittedLayers();
        m_textureMapper = nullptr;
        m_surface->willDestroyGLContext();
        m_context = nullptr;

        m_display.updateTimer = nullptr;
    });
    m_sceneState = nullptr;
    m_layerTreeHost = nullptr;
    m_surface->willDestroyCompositingRunLoop();
    m_compositingRunLoop = nullptr;
    m_surface = nullptr;
}

void ThreadedCompositor::suspend()
{
    ASSERT(RunLoop::isMain());
    m_surface->visibilityDidChange(false);

    if (++m_suspendedCount > 1)
        return;

    m_compositingRunLoop->suspend();
}

void ThreadedCompositor::resume()
{
    ASSERT(RunLoop::isMain());
    m_surface->visibilityDidChange(true);

    ASSERT(m_suspendedCount > 0);
    if (--m_suspendedCount > 0)
        return;

    m_compositingRunLoop->resume();
}

bool ThreadedCompositor::isActive() const
{
    return m_compositingRunLoop->isActive();
}

void ThreadedCompositor::backgroundColorDidChange()
{
    ASSERT(RunLoop::isMain());
    m_surface->backgroundColorDidChange();
}

void ThreadedCompositor::setSize(const IntSize& size, float deviceScaleFactor)
{
    ASSERT(RunLoop::isMain());
    Locker locker { m_attributes.lock };
    updateSceneAttributes(size, deviceScaleFactor);
}

void ThreadedCompositor::updateSceneState()
{
    if (!m_textureMapper)
        m_textureMapper = TextureMapper::create();

    auto reasons = OptionSet<CompositionReason>::all();
    m_sceneState->rootLayer().flushCompositingState(reasons);
    for (auto& layer : m_sceneState->committedLayers())
        layer->flushCompositingState(reasons);
}

void ThreadedCompositor::paintToCurrentGLContext(const TransformationMatrix& matrix, const IntSize& size)
{
    updateSceneState();

    FloatRect clipRect(FloatPoint { }, size);
    TextureMapperLayer& currentRootLayer = m_sceneState->rootLayer().ensureTarget();
    if (currentRootLayer.transform() != matrix)
        currentRootLayer.setTransform(matrix);

    bool sceneHasRunningAnimations = currentRootLayer.applyAnimationsRecursively(MonotonicTime::now());

    m_textureMapper->beginPainting(m_flipY ? TextureMapper::FlipY::Yes : TextureMapper::FlipY::No);
    m_textureMapper->beginClip(TransformationMatrix(), FloatRoundedRect(clipRect));

    m_surface->clear({ });

    WTFBeginSignpost(this, PaintTextureMapperLayerTree);
    currentRootLayer.paint(*m_textureMapper);
    WTFEndSignpost(this, PaintTextureMapperLayerTree);

    m_textureMapper->endClip();
    m_textureMapper->endPainting();

    if (sceneHasRunningAnimations)
        scheduleUpdate();
}

void ThreadedCompositor::renderLayerTree()
{
    ASSERT(m_sceneState);
    ASSERT(m_compositingRunLoop->isCurrent());

    if (m_suspendedCount > 0)
        return;

    if (!m_context || !m_context->makeContextCurrent())
        return;

    m_display.updateTimer->stop();

    IntSize viewportSize;
    float deviceScaleFactor;
    {
        Locker locker { m_attributes.lock };
        viewportSize = m_attributes.viewportSize;
        deviceScaleFactor = m_attributes.deviceScaleFactor;
        m_attributes.clientRendersNextFrame = m_sceneState->layersDidChange();
    }

    if (viewportSize.isEmpty())
        return;

    TransformationMatrix viewportTransform;
    viewportTransform.scale(deviceScaleFactor);

    m_surface->willRenderFrame(viewportSize);

    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }] {
        if (m_layerTreeHost)
            m_layerTreeHost->willRenderFrame();
    });

    WTFBeginSignpost(this, PaintToGLContext);
    paintToCurrentGLContext(viewportTransform, viewportSize);
    WTFEndSignpost(this, PaintToGLContext);

    updateFPSCounter();

    uint32_t compositionRequestID = m_compositionRequestID.load();
    UNUSED_VARIABLE(compositionRequestID);

    WTFEmitSignpost(this, DidRenderFrame, "compositionResponseID %i", compositionRequestID);

    m_context->swapBuffers();

    m_surface->didRenderFrame();

    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }] {
        if (m_layerTreeHost)
            m_layerTreeHost->didRenderFrame();
    });
}

uint32_t ThreadedCompositor::requestComposition()
{
    ASSERT(RunLoop::isMain());
    uint32_t compositionRequestID = ++m_compositionRequestID;
    scheduleUpdate();
    return compositionRequestID;
}

void ThreadedCompositor::scheduleUpdate()
{
    m_compositingRunLoop->scheduleUpdate();
}

RunLoop* ThreadedCompositor::runLoop()
{
    if (!m_compositingRunLoop)
        return nullptr;

    return &m_compositingRunLoop->runLoop();
}

void ThreadedCompositor::frameComplete()
{
    WTFEmitSignpost(this, FrameComplete);

    ASSERT(m_compositingRunLoop->isCurrent());
    displayUpdateFired();
    sceneUpdateFinished();
}

WebCore::DisplayRefreshMonitor& ThreadedCompositor::displayRefreshMonitor() const
{
    return m_displayRefreshMonitor.get();
}

void ThreadedCompositor::displayUpdateFired()
{
    m_display.displayUpdate = m_display.displayUpdate.nextUpdate();

    WebProcess::singleton().eventDispatcher().notifyScrollingTreesDisplayDidRefresh(m_display.displayID);

    m_display.updateTimer->startOneShot(Seconds { 1.0 / m_display.displayUpdate.updatesPerSecond });
}

void ThreadedCompositor::sceneUpdateFinished()
{
    bool shouldDispatchDisplayRefreshCallback = m_displayRefreshMonitor->requiresDisplayRefreshCallback(m_display.displayUpdate);

    if (!shouldDispatchDisplayRefreshCallback) {
        Locker locker { m_attributes.lock };
        shouldDispatchDisplayRefreshCallback |= m_attributes.clientRendersNextFrame;
    }

    Locker stateLocker { m_compositingRunLoop->stateLock() };

    if (shouldDispatchDisplayRefreshCallback)
        m_displayRefreshMonitor->dispatchDisplayRefreshCallback();

    m_compositingRunLoop->updateCompleted(stateLocker);
}

void ThreadedCompositor::updateSceneAttributes(const IntSize& size, float deviceScaleFactor)
{
    m_attributes.viewportSize = size;
    m_attributes.deviceScaleFactor = deviceScaleFactor;
    m_attributes.viewportSize.scale(m_attributes.deviceScaleFactor);
}

void ThreadedCompositor::initializeFPSCounter()
{
    const auto showFPSEnvironment = String::fromLatin1(getenv("WEBKIT_SHOW_FPS"));
    bool ok = false;
    Seconds interval(showFPSEnvironment.toDouble(&ok));
    if (ok && interval) {
        m_fpsCounter.exposesFPS = true;
        m_fpsCounter.calculationInterval = interval;
    }
}

void ThreadedCompositor::updateFPSCounter()
{
    if (!m_fpsCounter.exposesFPS)
        return;

    m_fpsCounter.frameCountSinceLastCalculation++;
    const Seconds delta = MonotonicTime::now() - m_fpsCounter.lastCalculationTimestamp;
    if (delta >= m_fpsCounter.calculationInterval) {
        WTFSetCounter(FPS, static_cast<int>(std::round(m_fpsCounter.frameCountSinceLastCalculation / delta.seconds())));
        if (m_fpsCounter.exposesFPS)
            m_fpsCounter.fps = m_fpsCounter.frameCountSinceLastCalculation / delta.seconds();
        m_fpsCounter.frameCountSinceLastCalculation = 0;
        m_fpsCounter.lastCalculationTimestamp += delta;
    } else if (m_fpsCounter.exposesFPS)
        m_fpsCounter.fps = std::nullopt;
}

}
#endif // USE(COORDINATED_GRAPHICS)
