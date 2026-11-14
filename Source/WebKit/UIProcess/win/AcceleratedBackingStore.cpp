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

#include "config.h"
#include "AcceleratedBackingStore.h"

#if USE(ACCELERATED_SURFACE)

#include "AcceleratedBackingStoreMessages.h"
#include "AcceleratedSurfaceMessages.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include <WebCore/BitmapInfo.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedBackingStore);

Ref<AcceleratedBackingStore> AcceleratedBackingStore::create(WebPageProxy& webPage, HWND window)
{
    return adoptRef(*new AcceleratedBackingStore(webPage, window));
}

AcceleratedBackingStore::AcceleratedBackingStore(WebPageProxy& webPage, HWND window)
    : m_webPage(webPage)
    , m_window(window)
{
}

AcceleratedBackingStore::~AcceleratedBackingStore()
{
    updateSurfaceID(0);
}

void AcceleratedBackingStore::updateSurfaceID(uint64_t surfaceID)
{
    if (m_surfaceID == surfaceID)
        return;

    if (m_surfaceID) {
        if (m_frameDonePending)
            frameDone();
        m_committedBufferID = std::nullopt;
        m_buffers.clear();
        if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
            legacyMainFrameProcess->removeMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID);
        m_legacyMainFrameProcess = nullptr;
    }

    m_surfaceID = surfaceID;
    if (m_surfaceID && m_webPage) {
        m_legacyMainFrameProcess = m_webPage->legacyMainFrameProcess();
        protect(*m_legacyMainFrameProcess)->addMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID, *this);
    }
}

void AcceleratedBackingStore::didCreateSHMBuffer(RendererBufferIdentifier id, WebCore::ShareableBitmap::Handle&& handle)
{
    RefPtr bitmap = WebCore::ShareableBitmap::create(WTF::move(handle), WebCore::SharedMemory::Protection::ReadOnly);
    if (!bitmap)
        return;

    m_buffers.set(id, bitmap.releaseNonNull());
}

void AcceleratedBackingStore::didDestroyBuffer(RendererBufferIdentifier id)
{
    m_buffers.remove(id);
    if (m_committedBufferID == id)
        m_committedBufferID = std::nullopt;
}

void AcceleratedBackingStore::frame(RendererBufferIdentifier bufferID, Rects&& damageRects)
{
    if (!m_buffers.contains(bufferID)) {
        frameDone();
        return;
    }

    auto previousBufferID = std::exchange(m_committedBufferID, bufferID);
    if (previousBufferID && *previousBufferID != bufferID)
        releaseBuffer(*previousBufferID);

    // The web process is told the frame is done once it has been painted, which throttles
    // rendering to the rate at which the view is actually updated.
    m_frameDonePending = true;

    if (damageRects.isEmpty())
        ::InvalidateRect(m_window, nullptr, FALSE);
    else {
        for (const auto& rect : damageRects) {
            RECT damage = rect;
            ::InvalidateRect(m_window, &damage, FALSE);
        }
    }

    // A window that is not visible receives no WM_PAINT, so don't wait for one.
    if (!::IsWindowVisible(m_window) || ::IsIconic(m_window))
        frameDone();
}

bool AcceleratedBackingStore::paint(HDC hdc, const WebCore::IntRect& dirtyRect)
{
    RefPtr<WebCore::ShareableBitmap> bitmap;
    if (m_committedBufferID)
        bitmap = m_buffers.get(*m_committedBufferID);

    bool painted = false;
    if (bitmap) {
        WebCore::IntRect bufferRect({ }, bitmap->size());
        auto rect = intersection(dirtyRect, bufferRect);
        if (!rect.isEmpty()) {
            auto bitmapInfo = WebCore::BitmapInfo::create(bitmap->size());
            bitmapInfo.bmiHeader.biWidth = bitmap->bytesPerRow() / 4;
            // The buffer rows are stored top to bottom.
            bitmapInfo.bmiHeader.biHeight = -bufferRect.height();
            ::SetDIBitsToDevice(hdc, rect.x(), rect.y(), rect.width(), rect.height(),
                rect.x(), rect.y(), 0, bufferRect.height(), bitmap->span().data(), &bitmapInfo, DIB_RGB_COLORS);
            painted = true;
        }
    }

    if (m_frameDonePending)
        frameDone();

    return painted;
}

void AcceleratedBackingStore::frameDone()
{
    m_frameDonePending = false;
    if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
        legacyMainFrameProcess->send(Messages::AcceleratedSurface::FrameDone(), m_surfaceID);
}

void AcceleratedBackingStore::releaseBuffer(RendererBufferIdentifier id)
{
    if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
        legacyMainFrameProcess->send(Messages::AcceleratedSurface::ReleaseBuffer(id), m_surfaceID);
}

} // namespace WebKit

#endif // USE(ACCELERATED_SURFACE)
