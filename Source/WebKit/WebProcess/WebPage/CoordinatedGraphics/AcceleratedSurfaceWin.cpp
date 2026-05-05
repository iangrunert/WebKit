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

#include "config.h"
#include "AcceleratedSurfaceWin.h"

#if USE(COORDINATED_GRAPHICS)
#include "AcceleratedBackingStoreMessages.h"
#include "AcceleratedSurfaceMessages.h"
#include "WebPage.h"
#include "WebProcess.h"
// Include order matters: gl3.h pulls in gl3platform.h which defines GL_APICALL,
// then gl2ext.h (which assumes GL_APICALL is already set) for GL_BGRA_EXT.
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <WebCore/Page.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedSurface);

static uint64_t generateID()
{
    static uint64_t identifier = 0;
    return ++identifier;
}

static uint64_t generateBufferID()
{
    static uint64_t identifier = 0;
    return ++identifier;
}

Ref<AcceleratedSurface> AcceleratedSurface::create(WebPage& webPage, Function<void()>&& frameCompleteHandler)
{
    return adoptRef(*new AcceleratedSurface(webPage, WTF::move(frameCompleteHandler)));
}

AcceleratedSurface::AcceleratedSurface(WebPage& webPage, Function<void()>&& frameCompleteHandler)
    : m_webPage(webPage)
    , m_frameCompleteHandler(WTF::move(frameCompleteHandler))
    , m_id(generateID())
{
    auto color = webPage.backgroundColor();
    m_backgroundColor = color ? color->toResolvedColorComponentsInColorSpace(WebCore::ColorSpace::SRGB) : white;
}

AcceleratedSurface::~AcceleratedSurface() = default;

void AcceleratedSurface::visibilityDidChange(bool)
{
    // Single-buffered SHM today: nothing to release on hide. The D3D11
    // SwapChain path will want to drop free buffers after a delay when
    // hidden, matching the GTK/PlayStation releaseUnusedBuffersTimer.
}

void AcceleratedSurface::backgroundColorDidChange()
{
    ASSERT(RunLoop::isMain());
    const auto& color = m_webPage->backgroundColor();
    m_backgroundColor = color ? color->toResolvedColorComponentsInColorSpace(WebCore::ColorSpace::SRGB) : white;
}

void AcceleratedSurface::didCreateCompositingRunLoop(WTF::RunLoop& runLoop)
{
    WebProcess::singleton().parentProcessConnection()->addMessageReceiver(runLoop, *this, Messages::AcceleratedSurface::messageReceiverName(), m_id);
}

void AcceleratedSurface::willDestroyCompositingRunLoop()
{
    WebProcess::singleton().parentProcessConnection()->removeMessageReceiver(Messages::AcceleratedSurface::messageReceiverName(), m_id);
    m_frameCompleteHandler = nullptr;
}

void AcceleratedSurface::willDestroyGLContext()
{
    destroyTarget();
}

void AcceleratedSurface::destroyTarget()
{
    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_colorRenderbuffer) {
        glDeleteRenderbuffers(1, &m_colorRenderbuffer);
        m_colorRenderbuffer = 0;
    }
    if (m_bitmap && m_currentBufferID) {
        // Tell the UI process to drop the bitmap.
        WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidDestroyBuffer(m_currentBufferID), m_id);
    }
    m_bitmap = nullptr;
    m_currentBufferID = 0;
    m_size = IntSize();
}

bool AcceleratedSurface::ensureTarget(const IntSize& size)
{
    if (m_fbo && m_size == size)
        return true;

    destroyTarget();

    if (size.isEmpty())
        return false;

    auto bitmap = ShareableBitmap::create({ size });
    if (!bitmap) {
        WTFLogAlways("AcceleratedSurfaceWin: failed to allocate ShareableBitmap %dx%d", size.width(), size.height());
        return false;
    }

    auto handle = bitmap->createReadOnlyHandle();
    if (!handle) {
        WTFLogAlways("AcceleratedSurfaceWin: failed to create ShareableBitmap handle");
        return false;
    }

    glGenRenderbuffers(1, &m_colorRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_colorRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size.width(), size.height());

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_colorRenderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        WTFLogAlways("AcceleratedSurfaceWin: framebuffer incomplete after attach");
        destroyTarget();
        return false;
    }

    m_size = size;
    m_bitmap = bitmap.releaseNonNull();
    m_currentBufferID = generateBufferID();

    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::DidCreateSHMBuffer(m_currentBufferID, WTF::move(*handle)), m_id);

    return true;
}

void AcceleratedSurface::willRenderFrame(const IntSize& size)
{
    if (!ensureTarget(size))
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_size.width(), m_size.height());
}

void AcceleratedSurface::clear(const OptionSet<WebCore::CompositionReason>&)
{
    if (!m_fbo)
        return;
    auto bg = m_backgroundColor.load();
    glClearColor(bg[0], bg[1], bg[2], bg[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}

void AcceleratedSurface::didRenderFrame()
{
    if (!m_fbo || !m_bitmap)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    // BGRA matches the Windows DIB pixel layout used in AcceleratedBackingStoreWin::paint
    // (SetDIBitsToDevice with BI_RGB expects B,G,R,A in memory order).
    glReadPixels(0, 0, m_size.width(), m_size.height(), GL_BGRA_EXT, GL_UNSIGNED_BYTE, m_bitmap->mutableSpan().data());

    sendFrameToUIProcess();
}

void AcceleratedSurface::sendFrameToUIProcess()
{
    if (!m_currentBufferID)
        return;

    Vector<IntRect, 1> damage;
    damage.append(IntRect({ }, m_size));

    m_isWaitingForFrameDone = true;
    WebProcess::singleton().parentProcessConnection()->send(Messages::AcceleratedBackingStore::Frame(m_currentBufferID, WTF::move(damage)), m_id);
}

void AcceleratedSurface::releaseBuffer(uint64_t)
{
    // Single-buffered SHM: the WebProcess clobbers the same bitmap each
    // frame and the UI side never holds a long-lived reference. Once the
    // D3D11 SwapChain path lands this releases the buffer back to the free
    // list so it can be re-rendered into.
}

void AcceleratedSurface::frameDone()
{
    m_isWaitingForFrameDone = false;
    if (m_frameCompleteHandler)
        m_frameCompleteHandler();
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
