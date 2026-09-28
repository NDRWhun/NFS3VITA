#include <lib/gliderenderer.h>
#include <lib/renderer.h>
#include <lib/glidetmu.h>
#include <lib/inputscript.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <cstddef>
#include <cstring>

namespace win32
{

static const x86::reg32 s_atlasSize = pf::gxm::AtlasSize;
static GlideRenderer* s_active = nullptr;

void GlideRenderer::endActiveFrame()
{
    if (s_active)
    {
        s_active->flush();
        s_active->m_frame = nullptr;
    }
}

void GlideRenderer::setActiveMapping(pf::gxm::Mapping mapping)
{
    if (s_active)
        s_active->setMapping(mapping);
}

GlideRenderer::GlideRenderer(Renderer* renderer)
    :   m_renderer(renderer)
    ,   m_frame(nullptr)
    ,   m_flushedVertices(0)
    ,   m_vertexState{ 1.f, 1.f, 1.f, float(s_atlasSize), 256.f, 0.f, 0.f }
    ,   m_zBuffer(true)
    ,   m_zTest(true)
    ,   m_alphaBlend(true)
    ,   m_mapping(pf::gxm::MapBox)
{
    m_tmus[0] = new GlideTMU(s_atlasSize);
    m_tmus[1] = nullptr;
    s_active = this;
}

GlideRenderer::~GlideRenderer()
{
    if (s_active == this)
    {
        flush();
        s_active = nullptr;
    }
    delete m_tmus[0];
}

// The frame currently being recorded.
pf::gxm::Frame& GlideRenderer::frame()
{
    return m_frame ? *m_frame : beginFrame();
}

// Tags the acquired frame for Glide, acquiring one only if nothing was submitted yet.
pf::gxm::Frame& GlideRenderer::beginFrame()
{
    pf::gxm::Device& device = pf::gxm::Device::get();
    pf::gxm::Frame* current = device.currentFrame();
    if (!current)
        current = &device.acquireFrame();
    if (current->kind != pf::gxm::FrameGlide)
    {
        InputScript::resume();
        current->kind = pf::gxm::FrameGlide;
        current->vblanks = 1;
        current->sourceWidth = m_renderer->m_width;
        current->sourceHeight = m_renderer->m_height;
    }
    m_frame = current;
    m_flushedVertices = current->drawCalls.empty() ? 0 : current->drawCalls.back().lastVertex;
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
    pf::gxm::Frame* f = m_frame;
    if (!f || f->vertexCount == m_flushedVertices)
        return;
    pf::gxm::DrawCall call;
    call.lastVertex = f->vertexCount;
    call.zTest = m_zTest;
    call.zWrite = m_zBuffer;
    call.alphaBlend = m_alphaBlend;
    call.mapping = m_mapping;
    f->drawCalls.push_back(call);
    if (m_mapping == pf::gxm::MapFull)
        f->wide = true;
    m_flushedVertices = f->vertexCount;
}

void GlideRenderer::swap()
{
    frame();
    flush();
    m_frame = nullptr;
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

void GlideRenderer::setMapping(pf::gxm::Mapping mapping)
{
    if (mapping != m_mapping)
    {
        flush();
        m_mapping = mapping;
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
    m_vertexState.colorCombine = colorFactor;
}

void GlideRenderer::setAlphaFactor(float alphaFactor)
{
    m_vertexState.alphaCombine = alphaFactor;
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
    m_vertexState.width = float(x86::reg32(256) >> (*info)->lod);
    m_vertexState.offX = float((*info)->x);
    m_vertexState.offY = float((*info)->y);
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

static_assert(sizeof(pf::gxm::Vertex) == 14 * sizeof(x86::reg32) && offsetof(pf::gxm::Vertex, color) == 12 &&
              offsetof(pf::gxm::Vertex, oow) == 36 && offsetof(pf::gxm::Vertex, offY) == 52, "vertex word layout");

static inline x86::reg32 word(float value)
{
    x86::reg32 result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

// Stores one vertex as 14 words in address order; the vertex buffer is uncached GPU memory.
void GlideRenderer::writeVertex(pf::gxm::Vertex* vertex, const GrVertex* v, const VertexState& state)
{
    volatile x86::reg32* out = reinterpret_cast<volatile x86::reg32*>(vertex);
    out[0] = word(v->x);
    out[1] = word(v->y);
    out[2] = word(v->ooz);
    out[3] = (x86::reg32(v->r) & 0xff)
           | (x86::reg32(v->g) & 0xff) << 8
           | (x86::reg32(v->b) & 0xff) << 16
           | x86::reg32(v->a) << 24;
    out[4] = word(v->tmuvtx[0].sow);
    out[5] = word(v->tmuvtx[0].tow);
    out[6] = word(state.colorCombine);
    out[7] = word(state.alphaCombine);
    out[8] = word(state.texWrap);
    out[9] = word(v->oow);
    out[10] = word(state.atlasSize);
    out[11] = word(state.width);
    out[12] = word(state.offX);
    out[13] = word(state.offY);
}

void GlideRenderer::drawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c)
{
    pf::gxm::Frame& f = frame();
    const x86::reg32 count = f.vertexCount;
    NFS2_ASSERT(count + 3 <= pf::gxm::MaxVertices);
    pf::gxm::Vertex* out = f.vertices + count;
    const VertexState state = m_vertexState;
    writeVertex(out, a, state);
    writeVertex(out + 1, b, state);
    writeVertex(out + 2, c, state);
    f.vertexCount = count + 3;
}

}
