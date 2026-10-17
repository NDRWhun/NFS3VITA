#ifndef LIB_GLIDERENDERER_H_
#define LIB_GLIDERENDERER_H_

#include <lib/winapp.h>
#include <winapi/types.h>
#include <vector>

namespace pf { namespace gxm { struct Frame; struct Vertex; struct Texture; enum Mapping : uint8_t; enum MipMode : uint8_t; } }

namespace win32
{

class Renderer;

enum TextureFormat
{
    TF_RGB_565,
    TF_ARGB_1555,
    TF_ARGB_4444,
};

enum AlphaBlend
{
    AB_1mSrcAlpha,
    AB_1
};

static const x86::reg32 RENDERER_NUM_TMU    = 2;

struct GrTmuVertex
{
    float  sow;
    float  tow;
    float  oow;
};

struct GrVertex
{
    float x, y, z;
    float r, g, b;
    float ooz;
    float a;
    float oow;
    GrTmuVertex  tmuvtx[RENDERER_NUM_TMU];
};

// Records Glide draw calls into gxm frames; each downloaded texture is its own GPU texture.
class GlideRenderer: public GenericResource
{
public:
    GlideRenderer(Renderer* renderer);
    ~GlideRenderer();

    void clear(x86::reg32 color);
    void swap();
    void render(x86::reg32 buffer);
    x86::reg32 getTextureMemSize(x86::reg32 tmu, x86::reg32 largeMipmapSize, TextureFormat format);
    void setZWrite(bool enable);
    void setZTest(bool enable);
    void setAlphaBlendDst(AlphaBlend method);
    void setColorFactor(float colorFactor);
    void setAlphaFactor(float alphaFactor);
    // largeLod/smallLod: the level range the game describes the texture with (GR_LOD_256 = 0 .. GR_LOD_1 = 8).
    void setTexture(x86::reg32 tmu, x86::reg32 address, x86::reg32 largeLod, x86::reg32 smallLod);
    void setTextureData(x86::reg32 tmu, x86::reg32 address, const void* data,
                        x86::reg32 largeLod, x86::reg32 smallLod, TextureFormat format);
    void drawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c);
    // Display area for the following triangles; kept across swaps until changed. A new renderer starts with MapBox.
    void setMapping(pf::gxm::Mapping mapping);
    // Texture addressing for the following triangles: clamp to the edge texels, or wrap (the initial state).
    void setTexClamp(bool clamp);
    // Nearest-texel sampling instead of bilinear filtering for the following triangles.
    void setTexPoint(bool point);
    // Mipmap use for the following triangles (grTexMipMapMode); a new renderer starts with MipNone.
    void setMipMode(pf::gxm::MipMode mode);

    // Closes the active renderer's open draw call and releases its frame before the frame is submitted elsewhere.
    static void endActiveFrame();
    // setMapping on the active renderer, if there is one.
    static void setActiveMapping(pf::gxm::Mapping mapping);

private:
    // Per-state fields of pf::gxm::Vertex, updated when the state changes.
    struct VertexState
    {
        float colorCombine, alphaCombine;
    };

    // A downloaded texture, indexed by its Glide address / getTextureMemSize().
    struct TextureEntry
    {
        pf::gxm::Texture*   texture;
        x86::reg32          largeLod;
        x86::reg32          smallLod;
        bool                opaque;     // every texel has alpha 255
    };

    pf::gxm::Frame& frame();
    pf::gxm::Frame& beginFrame();
    void flush();
    TextureEntry* textureEntry(x86::reg32 address, bool create);
    void bindTexture(const TextureEntry& entry);
    static void writeVertex(pf::gxm::Vertex* vertex, const GrVertex* v, const VertexState& state);

private:
    Renderer*               m_renderer;
    pf::gxm::Frame*         m_frame;            // Glide frame being recorded; null until the first Glide call after a submit
    x86::reg32              m_flushedVertices;
    VertexState             m_vertexState;
    bool                    m_zBuffer;
    bool                    m_zTest;
    bool                    m_alphaBlend;
    pf::gxm::Mapping        m_mapping;
    bool                    m_texClamp;
    bool                    m_texPoint;
    pf::gxm::MipMode        m_mipMode;
    x86::reg32              m_textureAddress;   // texture memory address last bound to TMU 0
    const pf::gxm::Texture* m_texture;          // texture of the open draw call
    bool                    m_textureOpaque;
    std::vector<TextureEntry> m_textures;
};

}

#endif
