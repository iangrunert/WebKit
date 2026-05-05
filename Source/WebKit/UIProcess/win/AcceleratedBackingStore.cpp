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

#include "config.h"
#include "AcceleratedBackingStore.h"

#if USE(COORDINATED_GRAPHICS)
#include "AcceleratedBackingStoreMessages.h"
#include "AcceleratedSurfaceMessages.h"
#include "LayerTreeContext.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include <WebCore/BitmapInfo.h>
#include <WebCore/Region.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedBackingStore);

Ref<AcceleratedBackingStore> AcceleratedBackingStore::create(WebPageProxy& webPage)
{
    return adoptRef(*new AcceleratedBackingStore(webPage));
}

AcceleratedBackingStore::AcceleratedBackingStore(WebPageProxy& webPage)
    : m_webPage(webPage)
{
}

AcceleratedBackingStore::~AcceleratedBackingStore()
{
    if (m_surfaceID) {
        if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
            legacyMainFrameProcess->removeMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID);
    }
}

void AcceleratedBackingStore::update(const LayerTreeContext& context)
{
    if (m_surfaceID == context.contextID)
        return;

    if (m_surfaceID) {
        sendFrameDoneIfNeeded();
        m_committedBitmap = nullptr;
        m_bitmaps.clear();
        if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
            legacyMainFrameProcess->removeMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID);
    }

    m_surfaceID = context.contextID;
    if (m_surfaceID && m_webPage) {
        m_legacyMainFrameProcess = m_webPage->legacyMainFrameProcess();
        Ref { *m_legacyMainFrameProcess }->addMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID, *this);
    }
}

void AcceleratedBackingStore::didCreateSHMBuffer(uint64_t id, ShareableBitmap::Handle&& handle)
{
    auto bitmap = ShareableBitmap::create(WTF::move(handle), SharedMemory::Protection::ReadOnly);
    if (!bitmap)
        return;
    m_bitmaps.set(id, WTF::move(bitmap));
}

void AcceleratedBackingStore::didDestroyBuffer(uint64_t id)
{
    if (m_committedBitmap && m_bitmaps.get(id) == m_committedBitmap.get())
        m_committedBitmap = nullptr;
    m_bitmaps.remove(id);
}

void AcceleratedBackingStore::frame(uint64_t id, Vector<IntRect, 1>&&)
{
    auto bitmap = m_bitmaps.get(id);
    if (!bitmap) {
        // Unknown buffer; ack so the WebProcess doesn't stall.
        sendFrameDoneIfNeeded();
        return;
    }

    m_committedBitmap = bitmap;
    m_hasPendingFrame = true;

    // Invalidate the whole window in logical/view coordinates. The bitmap is
    // at device-pixel resolution (viewSize * intrinsicDeviceScaleFactor) so
    // it can't be used directly as the invalidation rect; paint() then
    // resamples the bitmap onto the window via StretchDIBits.
    if (RefPtr webPage = m_webPage.get())
        webPage->setViewNeedsDisplay(IntRect({ }, webPage->viewSize()));

    // Phase 2: ack synchronously. The bitmap is shared memory and stays valid
    // until the next didCreateSHMBuffer replaces it, so we don't need to wait
    // for paint to complete before letting the WebProcess render the next frame.
    sendFrameDoneIfNeeded();
}

void AcceleratedBackingStore::sendFrameDoneIfNeeded()
{
    if (!m_hasPendingFrame)
        return;
    m_hasPendingFrame = false;
    if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
        legacyMainFrameProcess->send(Messages::AcceleratedSurface::FrameDone(), m_surfaceID);
}

void AcceleratedBackingStore::paint(HDC hdc, const IntRect& dirtyRect)
{
    if (!m_committedBitmap || dirtyRect.isEmpty())
        return;

    auto bitmapSize = m_committedBitmap->size();
    auto bitmapInfo = BitmapInfo::createBottomUp(bitmapSize);
    auto src = m_committedBitmap->mutableSpan().data();
    if (!src || bitmapSize.isEmpty())
        return;

    RefPtr webPage = m_webPage.get();
    if (!webPage)
        return;
    auto viewSize = webPage->viewSize();
    if (viewSize.isEmpty())
        return;

    // HDC and bitmap are both in physical pixels. The bitmap can transiently be
    // larger than the window during resize (compositor still rendering at the
    // old size), so clip blit dims to what fits — keeps a 1:1 blit (no
    // resampling) for the visible portion.
    auto dpr = webPage->intrinsicDeviceScaleFactor();
    int windowW = static_cast<int>(viewSize.width() * dpr);
    int windowH = static_cast<int>(viewSize.height() * dpr);
    int blitW = std::min(bitmapSize.width(), windowW);
    int blitH = std::min(bitmapSize.height(), windowH);
    if (blitW <= 0 || blitH <= 0)
        return;

    // BitmapInfo::createBottomUp uses negative biHeight (top-down DIB). For
    // top-down DIBs, SetDIBitsToDevice's ySrc is measured from the top of the
    // image. The top of the rendered page is at the top of the bitmap, so
    // ySrc = 0.
    SetDIBitsToDevice(hdc,
        /*xDest*/ 0, /*yDest*/ 0,
        /*w*/ blitW, /*h*/ blitH,
        /*xSrc*/ 0, /*ySrc*/ 0,
        /*StartScan*/ 0, /*cLines*/ bitmapSize.height(),
        src, &bitmapInfo, DIB_RGB_COLORS);
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
