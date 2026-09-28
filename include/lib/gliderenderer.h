#ifndef LIB_GLIDERENDERER_H_
#define LIB_GLIDERENDERER_H_

#include <lib/winapp.h>
#include <winapi/types.h>

namespace pf { namespace gxm { struct Frame; struct Vertex; enum Mapping : uint8_t; } }

namespace win32
{

class Renderer;
class GlideTMU;

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

// Records Glide draw calls into gxm frames; textures live in one shared atlas.
class GlideRenderer: public GenericResource
{
public:
    GlideRenderer(Renderer* renderer);
    ~GlideRenderer();

    void clear(x86::reg32 color);
    void swap();
    void render(x86::reg32 buffer);
    x86::reg32 textureMemStart(x86::reg32 tmu);
    x86::reg32 textureMemEnd(x86::reg32 tmu);
    x86::reg32 getTextureMemSize(x86::reg32 tmu, x86::reg32 largeMipmapSize, TextureFormat format);
    void setZWrite(bool enable);
    void setZTest(bool enable);
    void setAlphaBlendDst(AlphaBlend method);
    void setColorFactor(float colorFactor);
    void setAlphaFactor(float alphaFactor);
    void setTexture(x86::reg32 tmu, x86::reg32 address);
    void setTextureData(x86::reg32 tmu, x86::reg32 address, const void* data,
                        x86::reg32 largeMipmap, x86::reg32 smallMimmap, TextureFormat format);
    void drawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c);
    // Display area for the following triangles; kept across swaps until changed. A new renderer starts with MapBox.
    void setMapping(pf::gxm::Mapping mapping);

    // Closes the active renderer's open draw call and releases its frame before the frame is submitted elsewhere.
    static void endActiveFrame();
    // setMapping on the active renderer, if there is one.
    static void setActiveMapping(pf::gxm::Mapping mapping);

private:
    // Per-state fields of pf::gxm::Vertex, updated when the state changes.
    struct VertexState
    {
        float colorCombine, alphaCombine, texWrap;
        float atlasSize, width, offX, offY;
    };

    pf::gxm::Frame& frame();
    pf::gxm::Frame& beginFrame();
    void flush();
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
    GlideTMU*               m_tmus[2];
};

}

#endif
