/*
 * Copyright (C) 2026 Ian Grunert
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

#if USE(ACCELERATED_SURFACE)

#include "MessageReceiver.h"
#include "RendererBufferIdentifier.h"
#include <WebCore/IntRect.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>
#include <windows.h>

namespace WebKit {

class WebPageProxy;
class WebProcessProxy;

// Receives the frames rendered by the web process AcceleratedSurface into shared memory
// buffers, and paints the most recent one into the view window.
class AcceleratedBackingStore final : public IPC::MessageReceiver, public RefCounted<AcceleratedBackingStore> {
    WTF_MAKE_TZONE_ALLOCATED(AcceleratedBackingStore);
    WTF_MAKE_NONCOPYABLE(AcceleratedBackingStore);
public:
    using Rects = Vector<WebCore::IntRect, 1>;

    static Ref<AcceleratedBackingStore> create(WebPageProxy&, HWND);
    ~AcceleratedBackingStore();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    void updateSurfaceID(uint64_t);

    // Paints the area of the committed buffer that intersects the given rect, in device pixels.
    // Returns false if there is nothing to paint.
    bool paint(HDC, const WebCore::IntRect&);

private:
    AcceleratedBackingStore(WebPageProxy&, HWND);

    // IPC::MessageReceiver.
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;

    void didCreateSHMBuffer(RendererBufferIdentifier, WebCore::ShareableBitmapHandle&&);
    void didDestroyBuffer(RendererBufferIdentifier);
    void frame(RendererBufferIdentifier, Rects&&);
    void frameDone();
    void releaseBuffer(RendererBufferIdentifier);

    WeakPtr<WebPageProxy> m_webPage;
    HWND m_window { nullptr };
    uint64_t m_surfaceID { 0 };
    WeakPtr<WebProcessProxy> m_legacyMainFrameProcess;
    HashMap<RendererBufferIdentifier, Ref<WebCore::ShareableBitmap>> m_buffers;
    std::optional<RendererBufferIdentifier> m_committedBufferID;
    bool m_frameDonePending { false };
};

} // namespace WebKit

#endif // USE(ACCELERATED_SURFACE)
