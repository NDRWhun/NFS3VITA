#include <lib/gliderenderer.h>
#include <lib/renderer.h>
#include <lib/glidetmu.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <cstring>

namespace win32
{

static const x86::reg32 s_atlasSize = pf::gxm::AtlasSize;
static GlideRenderer* s_active = nullptr;

void GlideRenderer::flushActive()
{
    if (s_active)
        s_active->flush();
}

GlideRenderer::GlideRenderer(Renderer* renderer)
    :   m_renderer(renderer)
    ,   m_flushedVertices(0)
    ,   m_textureOffsetX(0)
    ,   m_textureOffsetY(0)
    ,   m_textureOffsetW(256)
    ,   m_colorFactor(1.f)
    ,   m_alphaFactor(1.f)
    ,   m_zBuffer(true)
    ,   m_zTest(true)
    ,   m_alphaBlend(true)
{
    m_tmus[0] = new GlideTMU(s_atlasSize);
    m_tmus[1] = nullptr;
    s_active = this;
}

GlideRenderer::~GlideRenderer()
{
    if (s_active == this)
        s_active = nullptr;
    delete m_tmus[0];
}

// The frame currently being recorded; acquires and tags a new one when needed.
pf::gxm::Frame& GlideRenderer::frame()
{
    pf::gxm::Device& device = pf::gxm::Device::get();
    pf::gxm::Frame* current = device.currentFrame();
    if (!current)
    {
        current = &device.acquireFrame();
        m_flushedVertices = 0;
    }
    if (current->kind != pf::gxm::FrameGlide)
    {
        static bool s_logged = false;
        if (!s_logged)
        {
            pf::logInfo("first glide frame");
            s_logged = true;
        }
        current->kind = pf::gxm::FrameGlide;
        current->vblanks = 1;
        current->sourceWidth = m_renderer->m_width;
        current->sourceHeight = m_renderer->m_height;
    }
    return *current;
}

void GlideRenderer::clear(x86::reg32 color)
{
    pf::gxm::Frame& f = frame();
    f.clear = true;
    f.clearColor = color;
}

void GlideRenderer::render(x86::reg32 buffer)
{
    m_renderer->m_currentBuffer = buffer;
}

// Closes the current draw call so following triangles get the new state.
void GlideRenderer::flush()
{
    pf::gxm::Frame* f = pf::gxm::Device::get().currentFrame();
    if (!f || f->kind != pf::gxm::FrameGlide || f->vertexCount == m_flushedVertices)
        return;
    pf::gxm::DrawCall call;
    call.lastVertex = f->vertexCount;
    call.zTest = m_zTest;
    call.zWrite = m_zBuffer;
    call.alphaBlend = m_alphaBlend;
    f->drawCalls.push_back(call);
    m_flushedVertices = f->vertexCount;
}

void GlideRenderer::swap()
{
    frame();
    flush();
    m_flushedVertices = 0;
    m_renderer->present();
}

x86::reg32 GlideRenderer::textureMemStart(x86::reg32 tmu)
{
    return m_tmus[tmu]->textureMemStart() + sizeof(GlTextureSlot*);
}

x86::reg32 GlideRenderer::textureMemEnd(x86::reg32 tmu)
{
    return m_tmus[tmu]->textureMemEnd();
}

x86::reg32 GlideRenderer::getTextureMemSize(x86::reg32 /*largeMipmapSize*/, x86::reg32 /*smallMipmapSize*/, TextureFormat /*format*/)
{
    return sizeof(GlTextureSlot*);
}

void GlideRenderer::setZWrite(bool enable)
{
    if (enable != m_zBuffer)
    {
        flush();
        m_zBuffer = enable;
    }
}

void GlideRenderer::setZTest(bool enable)
{
    if (enable != m_zTest)
    {
        flush();
        m_zTest = enable;
    }
}

void GlideRenderer::setAlphaBlendDst(AlphaBlend method)
{
    if (m_alphaBlend != (method == AB_1mSrcAlpha))
    {
        flush();
        m_alphaBlend = method == AB_1mSrcAlpha;
    }
}

void GlideRenderer::setColorFactor(float colorFactor)
{
    m_colorFactor = colorFactor;
}

void GlideRenderer::setAlphaFactor(float alphaFactor)
{
    m_alphaFactor = alphaFactor;
}

void GlideRenderer::setTexture(x86::reg32 tmu, x86::reg32 address)
{
    NFS2_ASSERT(tmu == 0);
    GlTextureSlot** info = m_tmus[tmu]->getTextureInfo(address);
    if (!*info)
    {
        pf::logError("texture at 0x%x used before download", address);
        return;
    }
    m_textureOffsetW = 256 >> (*info)->lod;
    m_textureOffsetX = (*info)->x;
    m_textureOffsetY = (*info)->y;
}

// Converts the largest mip level into the atlas as ABGR words.
void GlideRenderer::setTextureData(x86::reg32 tmu, x86::reg32 address, const void* data,
                                   x86::reg32 largeMipmap, x86::reg32 smallMipmap, TextureFormat format)
{
    NFS2_USE(smallMipmap);
    NFS2_ASSERT(tmu == 0);
    x86::reg32 size = 256 >> largeMipmap;

    pf::gxm::Device& device = pf::gxm::Device::get();
    GlTextureSlot** info = m_tmus[tmu]->getTextureInfo(address);
    if (*info)
    {
        m_tmus[tmu]->returnTextureSlot(*info, device.recordingFrameIndex());
    }
    *info = m_tmus[tmu]->reserveTextureSlot(largeMipmap, device.completedFrameIndex());

    const x86::reg16* pixels = reinterpret_cast<const x86::reg16*>(data);
    uint32_t* atlas = device.atlas() + (*info)->y * s_atlasSize + (*info)->x;
    for (x86::reg32 y = 0; y < size; ++y, atlas += s_atlasSize, pixels += size)
    {
        for (x86::reg32 x = 0; x < size; ++x)
        {
            x86::reg16 p = pixels[x];
            x86::reg32 r, g, b, a;
            switch (format)
            {
            case TF_RGB_565:
                r = (p >> 11) & 0x1f; r = (r << 3) | (r >> 2);
                g = (p >> 5) & 0x3f;  g = (g << 2) | (g >> 4);
                b = p & 0x1f;         b = (b << 3) | (b >> 2);
                a = 0xff;
                break;
            case TF_ARGB_1555:
                r = (p >> 10) & 0x1f; r = (r << 3) | (r >> 2);
                g = (p >> 5) & 0x1f;  g = (g << 3) | (g >> 2);
                b = p & 0x1f;         b = (b << 3) | (b >> 2);
                a = (p & 0x8000) ? 0xff : 0x00;
                break;
            case TF_ARGB_4444:
            default:
                r = (p >> 8) & 0xf;  r |= r << 4;
                g = (p >> 4) & 0xf;  g |= g << 4;
                b = p & 0xf;         b |= b << 4;
                a = (p >> 12) & 0xf; a |= a << 4;
                break;
            }
            atlas[x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }
}

void GlideRenderer::writeVertex(const GrVertex* v)
{
    pf::gxm::Frame& f = frame();
    NFS2_ASSERT(f.vertexCount < pf::gxm::MaxVertices);
    pf::gxm::Vertex& out = f.vertices[f.vertexCount++];
    out.x = v->x;
    out.y = v->y;
    out.z = v->ooz;
    out.color = (x86::reg8)(v->r)
              | (x86::reg8)(v->g) << 8
              | (x86::reg8)(v->b) << 16
              | (x86::reg8)(v->a) << 24;
    out.u = v->tmuvtx[0].sow;
    out.v = v->tmuvtx[0].tow;
    out.colorCombine = m_colorFactor;
    out.alphaCombine = m_alphaFactor;
    out.texWrap = 1.f;
    out.oow = v->oow;
    out.atlasSize = float(s_atlasSize);
    out.width = float(m_textureOffsetW);
    out.offX = float(m_textureOffsetX);
    out.offY = float(m_textureOffsetY);
}

void GlideRenderer::drawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c)
{
    writeVertex(a);
    writeVertex(b);
    writeVertex(c);
}

}
