#include <lib/gliderenderer.h>
#include <lib/renderer.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <cstddef>
#include <cstring>

namespace win32
{

static GlideRenderer* s_active = nullptr;
// Glide texture memory taken per texture; textures are addressed by index (address / this).
static const x86::reg32 s_textureHandleSize = 4;
static const x86::reg32 s_maxTextures = 65536;

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
    ,   m_vertexState{ 1.f, 1.f }
    ,   m_zBuffer(true)
    ,   m_zTest(true)
    ,   m_alphaBlend(true)
    ,   m_mapping(pf::gxm::MapBox)
    ,   m_texClamp(false)
    ,   m_texPoint(false)
    ,   m_mipMode(pf::gxm::MipNone)
    ,   m_textureAddress(0)
    ,   m_texture(nullptr)
{
    s_active = this;
}

GlideRenderer::~GlideRenderer()
{
    if (s_active == this)
    {
        flush();
        s_active = nullptr;
    }
    pf::gxm::Device& device = pf::gxm::Device::get();
    for (TextureEntry& entry : m_textures)
    {
        if (entry.texture)
            device.releaseTexture(entry.texture);
    }
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
    call.texClamp = m_texClamp;
    call.texPoint = m_texPoint;
    call.mipMode = m_mipMode;
    call.texture = m_texture;
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

x86::reg32 GlideRenderer::getTextureMemSize(x86::reg32 /*largeMipmapSize*/, x86::reg32 /*smallMipmapSize*/, TextureFormat /*format*/)
{
    return s_textureHandleSize;
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

void GlideRenderer::setTexClamp(bool clamp)
{
    if (clamp != m_texClamp)
    {
        flush();
        m_texClamp = clamp;
    }
}

void GlideRenderer::setTexPoint(bool point)
{
    if (point != m_texPoint)
    {
        flush();
        m_texPoint = point;
    }
}

void GlideRenderer::setMipMode(pf::gxm::MipMode mode)
{
    if (mode != m_mipMode)
    {
        flush();
        m_mipMode = mode;
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

// The entry of a Glide texture address, created if asked; null if there is none or the address is out of range.
GlideRenderer::TextureEntry* GlideRenderer::textureEntry(x86::reg32 address, bool create)
{
    const x86::reg32 index = address / s_textureHandleSize;
    if (index >= m_textures.size())
    {
        if (!create || index >= s_maxTextures)
            return nullptr;
        m_textures.resize(index + 1, TextureEntry{ nullptr, 0, 0 });
    }
    return &m_textures[index];
}

// Samples the following triangles from the entry's texture.
void GlideRenderer::bindTexture(const TextureEntry& entry)
{
    if (entry.texture != m_texture)
    {
        flush();
        m_texture = entry.texture;
    }
}

void GlideRenderer::setTexture(x86::reg32 tmu, x86::reg32 address, x86::reg32 largeLod, x86::reg32 smallLod)
{
    NFS2_ASSERT(tmu == 0);
    m_textureAddress = address;
    const TextureEntry* entry = textureEntry(address, false);
    if (!entry || !entry->texture)
    {
        pf::logError("texture at 0x%x used before download", address);
        return;
    }
    static bool logged = false;
    if (!logged && (largeLod != entry->largeLod || smallLod != entry->smallLod))
    {
        logged = true;
        pf::logInfo("glide: texture at 0x%x sourced with levels %u..%u, downloaded with %u..%u (using the download)",
                    address, unsigned(largeLod), unsigned(smallLod), unsigned(entry->largeLod), unsigned(entry->smallLod));
    }
    bindTexture(*entry);
}

// Copies all downloaded mip levels into a new GPU texture; the previous texture at the address is released.
void GlideRenderer::setTextureData(x86::reg32 tmu, x86::reg32 address, const void* data,
                                   x86::reg32 largeLod, x86::reg32 smallLod, TextureFormat format)
{
    NFS2_ASSERT(tmu == 0);
    NFS2_ASSERT(largeLod <= smallLod && smallLod <= 8);
    const x86::reg32 size = 256 >> largeLod;
    const x86::reg32 levels = smallLod - largeLod + 1;
    const x86::reg16* texels = static_cast<const x86::reg16*>(data);

    if (levels > 1)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            pf::logInfo("glide: first texture with mip levels: %u² with %u levels", unsigned(size), unsigned(levels));
        }
    }

    pf::gxm::TextureFormat gxmFormat = pf::gxm::TextureArgb4444;
    if (format == TF_RGB_565)
        gxmFormat = pf::gxm::TextureRgb565;
    else if (format == TF_ARGB_1555)
        gxmFormat = pf::gxm::TextureArgb1555;

    pf::gxm::Device& device = pf::gxm::Device::get();
    TextureEntry* entry = textureEntry(address, true);
    if (!entry)
    {
        pf::logError("texture address 0x%x out of range, download ignored", address);
        return;
    }
    // Bound: last selected, or still drawn from after a select of an address without a texture.
    const bool bound = address == m_textureAddress || (entry->texture && entry->texture == m_texture);
    if (bound)
        flush();
    if (entry->texture)
        device.releaseTexture(entry->texture);
    entry->texture = device.createTexture(gxmFormat, size, levels, texels);
    entry->largeLod = largeLod;
    entry->smallLod = smallLod;
    // A bound texture that is downloaded again is drawn from its new data.
    if (bound)
        bindTexture(*entry);
}

static_assert(sizeof(pf::gxm::Vertex) == 9 * sizeof(x86::reg32) && offsetof(pf::gxm::Vertex, color) == 12 &&
              offsetof(pf::gxm::Vertex, sow) == 16 && offsetof(pf::gxm::Vertex, oow) == 32, "vertex word layout");

static inline x86::reg32 word(float value)
{
    x86::reg32 result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

// Stores one vertex as 9 words in address order; the vertex buffer is uncached GPU memory.
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
    out[8] = word(v->oow);
}

// Triangles past the frame's vertex buffer are dropped.
void GlideRenderer::drawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c)
{
    pf::gxm::Frame& f = frame();
    const x86::reg32 count = f.vertexCount;
    if (__builtin_expect(count + 3 > pf::gxm::MaxVertices, 0))
    {
        logDropped();
        return;
    }
    pf::gxm::Vertex* out = f.vertices + count;
    const VertexState state = m_vertexState;
    writeVertex(out, a, state);
    writeVertex(out + 1, b, state);
    writeVertex(out + 2, c, state);
    f.vertexCount = count + 3;
}

GlideRenderer* GlideRenderer::active()
{
    return s_active;
}

// Triangles that do not fit in the frame's vertex buffer go to a scratch area and are not drawn.
pf::gxm::Vertex* GlideRenderer::appendTriangles(x86::reg32 count)
{
    pf::gxm::Frame& f = frame();
    const x86::reg32 first = f.vertexCount;
    const x86::reg64 vertices = 3 * x86::reg64(count);
    if (__builtin_expect(first + vertices > pf::gxm::MaxVertices, 0))
    {
        logDropped();
        m_scratch.resize(size_t(vertices));
        return m_scratch.data();
    }
    f.vertexCount = first + x86::reg32(vertices);
    return f.vertices + first;
}

void GlideRenderer::logDropped()
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        pf::logError("glide: vertex buffer full (%u vertices), triangles dropped", unsigned(pf::gxm::MaxVertices));
    }
}

}
