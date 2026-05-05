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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
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
#include <WebCore/IntRect.h>
#include <WebCore/IntSize.h>
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>
#include <windows.h>

namespace WebCore {
class ShareableBitmap;
class ShareableBitmapHandle;
}

namespace WebKit {
class LayerTreeContext;
class WebPageProxy;
class WebProcessProxy;

// Phase 2: receives ShareableBitmap-backed frames from AcceleratedSurfaceWin in
// the WebProcess and BitBlts them into the host HWND DC. Phase 3 will replace
// this with a DirectComposition visual fed by D3D11 shared NT handles.
class AcceleratedBackingStore final : public IPC::MessageReceiver, public RefCounted<AcceleratedBackingStore> {
    WTF_MAKE_TZONE_ALLOCATED(AcceleratedBackingStore);
public:
    static Ref<AcceleratedBackingStore> create(WebPageProxy&);
    ~AcceleratedBackingStore();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    void update(const LayerTreeContext&);
    void paint(HDC, const WebCore::IntRect& dirtyRect);

private:
    explicit AcceleratedBackingStore(WebPageProxy&);

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;

    // Message handlers (from AcceleratedBackingStore.messages.in)
    void didCreateSHMBuffer(uint64_t id, WebCore::ShareableBitmapHandle&&);
    void didDestroyBuffer(uint64_t id);
    void frame(uint64_t id, Vector<WebCore::IntRect, 1>&& damage);

    void sendFrameDoneIfNeeded();

    WeakPtr<WebPageProxy> m_webPage;
    WeakPtr<WebProcessProxy> m_legacyMainFrameProcess;
    uint64_t m_surfaceID { 0 };
    HashMap<uint64_t, RefPtr<WebCore::ShareableBitmap>> m_bitmaps;
    RefPtr<WebCore::ShareableBitmap> m_committedBitmap;
    bool m_hasPendingFrame { false };
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
