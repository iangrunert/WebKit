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
#include "CompositingRunLoop.h"
#include "ThreadedDisplayRefreshMonitorWin.h"
#include <WebCore/DisplayUpdate.h>
#include <WebCore/GLContext.h>
#include <WebCore/IntSize.h>
#include <atomic>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/CheckedPtr.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WebCore {
class TextureMapper;
class TransformationMatrix;
}

namespace WebKit {
class AcceleratedSurface;
class CoordinatedSceneState;
class LayerTreeHost;

class ThreadedCompositor : public ThreadSafeRefCounted<ThreadedCompositor>, public CanMakeThreadSafeCheckedPtr<ThreadedCompositor> {
    WTF_MAKE_TZONE_ALLOCATED(ThreadedCompositor);
    WTF_MAKE_NONCOPYABLE(ThreadedCompositor);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ThreadedCompositor);
public:
    static Ref<ThreadedCompositor> create(LayerTreeHost&, ThreadedDisplayRefreshMonitor::Client&, WebCore::PlatformDisplayID);
    virtual ~ThreadedCompositor();

    uint64_t surfaceID() const;
    int maxTextureSize() const { return m_maxTextureSize; }

    void backgroundColorDidChange();

    void setSize(const WebCore::IntSize&, float);
    uint32_t requestComposition();
    void scheduleUpdate();
    RunLoop* runLoop();

    void invalidate();

    WebCore::DisplayRefreshMonitor& displayRefreshMonitor() const;

    void suspend();
    void resume();

    bool isActive() const;

    std::optional<float> fps() const { return m_fpsCounter.fps.load(); };

private:
    ThreadedCompositor(LayerTreeHost&, ThreadedDisplayRefreshMonitor::Client&, WebCore::PlatformDisplayID);

    void updateSceneState();
    void renderLayerTree();
    void paintToCurrentGLContext(const WebCore::TransformationMatrix&, const WebCore::IntSize&);
    void frameComplete();

    void displayUpdateFired();
    void sceneUpdateFinished();

    void updateSceneAttributes(const WebCore::IntSize&, float deviceScaleFactor);

    void initializeFPSCounter();
    void updateFPSCounter();

    CheckedPtr<LayerTreeHost> m_layerTreeHost;
    RefPtr<AcceleratedSurface> m_surface;
    RefPtr<CoordinatedSceneState> m_sceneState;
    std::unique_ptr<WebCore::GLContext> m_context;

    bool m_flipY { false };
    int m_maxTextureSize { 0 };
    std::atomic<unsigned> m_suspendedCount { 0 };

    std::unique_ptr<CompositingRunLoop> m_compositingRunLoop;

    struct {
        Lock lock;
        WebCore::IntSize viewportSize;
        float deviceScaleFactor { 1 };
        bool clientRendersNextFrame { false };
    } m_attributes;

    std::unique_ptr<WebCore::TextureMapper> m_textureMapper;

    struct {
        bool exposesFPS { false };
        Seconds calculationInterval { 1_s };
        MonotonicTime lastCalculationTimestamp;
        unsigned frameCountSinceLastCalculation { 0 };
        std::atomic<std::optional<float>> fps;
    } m_fpsCounter;

    std::atomic<uint32_t> m_compositionRequestID { 0 };

    struct {
        WebCore::PlatformDisplayID displayID;
        WebCore::DisplayUpdate displayUpdate;
        std::unique_ptr<RunLoop::Timer> updateTimer;
    } m_display;

    const Ref<ThreadedDisplayRefreshMonitor> m_displayRefreshMonitor;
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
