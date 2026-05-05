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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(COORDINATED_GRAPHICS)

#include "MessageReceiver.h"
#include <WebCore/ColorComponents.h>
#include <WebCore/ColorModels.h>
#include <WebCore/CoordinatedCompositionReason.h>
#include <WebCore/IntRect.h>
#include <WebCore/IntSize.h>
#include <atomic>
#include <wtf/Function.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/WeakRef.h>

namespace WTF {
class RunLoop;
}

namespace WebCore {
class ShareableBitmap;
}

namespace WebKit {
class WebPage;

// Phase 2: SHM render path. The threaded compositor renders into an offscreen
// FBO (RGBA8 renderbuffer); on didRenderFrame we glReadPixels into a
// ShareableBitmap and send a Frame IPC to AcceleratedBackingStoreWin in the UI
// process, which BitBlts the bitmap into the HWND DC.
// Phase 3 will replace the SHM render target with ANGLE D3D11 shared NT
// handles + DirectComposition (see silly-snuggling-scone-d3d-present.md).
class AcceleratedSurface final
    : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<AcceleratedSurface, WTF::DestructionThread::MainRunLoop>
    , public IPC::MessageReceiver {
    WTF_MAKE_TZONE_ALLOCATED(AcceleratedSurface);
public:
    static Ref<AcceleratedSurface> create(WebPage&, Function<void()>&& frameCompleteHandler);
    ~AcceleratedSurface();

    using ColorComponents = WebCore::ColorComponents<float, 4>;

    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

    uint64_t window() const { return 0; }
    uint64_t surfaceID() const { return m_id; }
    // No FlipY: glReadPixels reads bottom-up, and we render into a
    // BottomUp DIB (BitmapInfo::createBottomUp), so the two orientations
    // already match without flipping.
    bool shouldPaintMirrored() const { return false; }

    void willDestroyGLContext();
    void willRenderFrame(const WebCore::IntSize&);
    void didRenderFrame();
    void clear(const OptionSet<WebCore::CompositionReason>&);

    void didCreateCompositingRunLoop(WTF::RunLoop&);
    void willDestroyCompositingRunLoop();

    void visibilityDidChange(bool);
    void backgroundColorDidChange();

private:
    AcceleratedSurface(WebPage&, Function<void()>&& frameCompleteHandler);

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;

    // Message handlers
    void releaseBuffer(uint64_t bufferID);
    void frameDone();

    void destroyTarget();
    bool ensureTarget(const WebCore::IntSize&);
    void sendFrameToUIProcess();

    static constexpr ColorComponents white { 1.f, 1.f, 1.f, WebCore::AlphaTraits<float>::opaque };

    WeakRef<WebPage> m_webPage;
    Function<void()> m_frameCompleteHandler;
    uint64_t m_id { 0 };
    WebCore::IntSize m_size;
    bool m_isVisible { false };
    std::atomic<ColorComponents> m_backgroundColor { white };

    // Phase 2 SHM render target. Created lazily once we know the viewport size.
    uint64_t m_currentBufferID { 0 };
    unsigned m_fbo { 0 };
    unsigned m_colorRenderbuffer { 0 };
    RefPtr<WebCore::ShareableBitmap> m_bitmap;
    bool m_bitmapHandleSent { false };
    bool m_isWaitingForFrameDone { false };
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
