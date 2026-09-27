#include <lib/renderer.h>
#include <lib/window.h>
#include <lib/gamepad.h>
#include <lib/gliderenderer.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <cstring>

namespace win32
{

Renderer::Renderer(WinApplication* application, Window *window)
    :   m_application(application)
    ,   m_window(window)
    ,   m_videoMemory(new MemMap(pf::gxm::MaxSourceWidth * pf::gxm::MaxSourceHeight * 2 * 2)) // double buffer 16 bits 800x600
    ,   m_currentBuffer(0)
    ,   m_width(640)
    ,   m_height(480)
    ,   m_depth(16)
    ,   m_colorPalette()
{
}

Renderer::~Renderer()
{
    delete m_videoMemory;
}

void Renderer::setVideoMode(x86::reg32 w, x86::reg32 h, x86::reg32 bpp)
{
    NFS2_ASSERT(w <= pf::gxm::MaxSourceWidth && h <= pf::gxm::MaxSourceHeight);
    m_width = w;
    m_height = h;
    m_depth = bpp;
    pf::logInfo("video mode %ux%u %u bpp", w, h, bpp);
#ifdef NFS3_DEBUG_INSTALLWIN
    // Parsed install.win table: 40 slots of 128 bytes at guest 0x7a2634.
    for (x86::reg32 i = 0; i < 26; ++i)
        pf::logInfo("install.win[%u] = '%s'", i, &m_application->getMemory<char>(0x7a2634 + i * 128));
#endif
}

void Renderer::updatePalette(x86::reg32 colorCount, const x86::reg32* colors)
{
    NFS2_ASSERT(colorCount <= 256);
    memcpy(m_colorPalette, colors, colorCount*4);
}

x86::reg32 Renderer::getFrontBuffer() const
{
    return m_videoMemory->getBlockStart() + m_currentBuffer * m_width * m_height * 2;
}

x86::reg32 Renderer::getBackBuffer() const
{
    return m_videoMemory->getBlockStart() + (1 - m_currentBuffer) * m_width * m_height * 2;
}

// Submits the recorded Glide frame.
void Renderer::present()
{
    Gamepad::updateKeys();
    pf::gxm::Device::get().submitFrame();
}

// Presents the guest front buffer.
void Renderer::update(x86::reg32 vblanks)
{
    pf::gxm::Device& device = pf::gxm::Device::get();
    pf::gxm::Frame* pending = device.currentFrame();
    if (pending && pending->kind == pf::gxm::FrameGlide)
    {
        GlideRenderer::flushActive();
        device.submitFrame();
    }

    pf::gxm::Frame& frame = device.acquireFrame();
    frame.kind = m_depth == 16 ? pf::gxm::FrameFramebuffer16 : pf::gxm::FrameFramebuffer8;
    frame.vblanks = vblanks;
    frame.sourceWidth = m_width;
    frame.sourceHeight = m_height;
    const x86::reg8* source = &m_application->getMemory<x86::reg8>(getFrontBuffer());
    if (m_depth == 16)
    {
        memcpy(frame.framebuffer, source, m_width * m_height * 2);
    }
    else
    {
        memcpy(frame.framebuffer, source, m_width * m_height);
        // Palette entries arrive as R<<24|G<<16|B<<8|A; the texture palette wants ABGR words.
        for (x86::reg32 i = 0; i < 256; ++i)
        {
            x86::reg32 c = m_colorPalette[i];
            frame.palette[i] = 0xff000000u | ((c >> 8) & 0xff) << 16 | ((c >> 16) & 0xff) << 8 | (c >> 24);
        }
    }
    Gamepad::updateKeys();
    device.submitFrame();
}

x86::reg32 Renderer::lock(x86::reg32 index)
{
    return index == 0 ? getFrontBuffer() : getBackBuffer();
}

void Renderer::unlock(x86::reg32 index)
{
    if (index == 0)
    {
        update(1);
    }
}

void Renderer::swap()
{
    m_currentBuffer = 1 - m_currentBuffer;
    // Flip pacing: the original path presented twice at 60 Hz.
    update(2);
}

}
