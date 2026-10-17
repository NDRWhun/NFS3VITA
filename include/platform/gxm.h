#ifndef PLATFORM_GXM_H_
#define PLATFORM_GXM_H_

#include <cstdint>
#include <vector>

namespace pf { namespace gxm {

enum
{
    DisplayWidth    = 960,
    DisplayHeight   = 544,
    MaxVertices     = 32000,
    MaxSourceWidth  = 1024,     // largest Glide resolution (grSstWinOpen)
    MaxSourceHeight = 768,
    FrameSlots      = 3
};

// Glide triangle vertex as consumed by glide_v.cg (36 bytes).
struct Vertex
{
    float       x, y, z;        // Glide screen position, z = ooz
    uint32_t    color;
    float       sow, tow;       // texture coordinates (0..256 over the texture) times oow
    float       colorCombine, alphaCombine, oow;
};

// Texel layout of a Glide texture, 16 bits per texel.
enum TextureFormat : uint8_t
{
    TextureRgb565,
    TextureArgb1555,
    TextureArgb4444
};

// Mipmap use of a draw (grTexMipMapMode).
enum MipMode : uint8_t
{
    MipNone,        // the largest level only
    MipNearest,     // the nearest level
    MipBlend        // blend between the two nearest levels
};

// A GPU texture created by Device::createTexture; opaque outside the device.
struct Texture;

// Display area that Glide screen coordinates (0..sourceWidth, 0..sourceHeight) are mapped to.
enum Mapping : uint8_t
{
    MapBox,     // the source image's aspect fitted into the display (4:3: the game area), or the display (setStretch)
    MapFull     // the whole display (widescreen world views)
};

struct DrawCall
{
    uint32_t    lastVertex;
    bool        zTest;
    bool        zWrite;
    bool        alphaBlend;
    Mapping     mapping;
    bool        texClamp;       // texture coordinates clamp to the edge texels (else they wrap)
    bool        texPoint;       // nearest texel (else bilinear filtering)
    MipMode     mipMode;
    const Texture* texture;
};

enum FrameKind
{
    FrameEmpty,
    FrameFramebuffer16,     // 16-bit RGB565 guest framebuffer
    FrameFramebuffer8,      // 8-bit paletted guest framebuffer
    FrameGlide              // recorded triangles
};

// One frame of work handed from the game thread to the render thread.
struct Frame
{
    FrameKind               kind;
    uint32_t                vblanks;        // display hold: 1 = 60 Hz, 2 = 30 Hz
    uint32_t                frameIndex;
    uint32_t                sourceWidth;    // game resolution
    uint32_t                sourceHeight;
    bool                    clear;
    uint32_t                clearColor;     // 0x00RRGGBB
    bool                    wide;           // a draw call uses MapFull
    uint32_t                vertexCount;
    std::vector<DrawCall>   drawCalls;
    // GPU-visible buffers owned by the device, valid while the frame is acquired.
    Vertex*                 vertices;
    uint8_t*                framebuffer;
    uint32_t*               palette;
};

struct Rect
{
    int x, y, w, h;
};

class Device
{
public:
    static Device& get();

    bool init();
    void shutdown();

    // Game thread: frames are acquired, filled, then submitted; at most FrameSlots in flight.
    // acquireFrame() waits until the render thread and the GPU are done with the next slot, so callers
    // release the execution lock around it; it returns the acquired frame again until submitFrame().
    Frame& acquireFrame();
    Frame* currentFrame();
    void submitFrame();

    // Game thread: a square texture of size² texels with levels mip levels. texels holds the levels in Glide
    // order (largest first, rows top to bottom, 16 bits per texel). When texture memory is full it reuses
    // released textures that frames in flight may still read (logged); fatal if that is not enough either.
    Texture* createTexture(TextureFormat format, uint32_t size, uint32_t levels, const uint16_t* texels);
    // Game thread: frees the texture once the GPU has finished every frame recorded so far.
    void releaseTexture(Texture* texture);
    // Frames submitted so far; readable from any thread.
    uint32_t submittedFrameCount() const;

    // Glide frames fill the display whatever their aspect; framebuffer frames (movies) keep theirs. Before init.
    void setStretch(bool stretch);
    // Vita3K reads the 565 swizzle the other way round; hardware wants RGB.
    void setFramebuffer16Bgr(bool bgr);
    // The 4:3 game area centred on the display.
    Rect viewport() const;

private:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    struct Impl;
    Impl*       m_impl;
};

}}

#endif
