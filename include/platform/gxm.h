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
    AtlasSize       = 2048,
    MaxSourceWidth  = 1024,     // largest Glide resolution (grSstWinOpen)
    MaxSourceHeight = 768,
    FrameSlots      = 3
};

// Glide triangle vertex as consumed by glide_v.cg (56 bytes); glide_v does not read the reserved words.
struct Vertex
{
    float       x, y, z;
    uint32_t    color;
    float       u, v;
    float       colorCombine, alphaCombine, reserved0, oow;
    float       reserved1, width, offX, offY;
};

// Display area that Glide screen coordinates (0..sourceWidth, 0..sourceHeight) are mapped to.
enum Mapping : uint8_t
{
    MapBox,     // the 4:3 game area, Device::viewport()
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
    bool        texPoint;       // clamped textures use the nearest texel (else bilinear filtering)
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

    uint32_t* atlas() const { return m_atlas; }
    uint32_t completedFrameIndex() const;
    uint32_t recordingFrameIndex() const;
    // Frames submitted so far; readable from any thread.
    uint32_t submittedFrameCount() const;

    void setStretch(bool stretch) { m_stretch = stretch; }
    // Vita3K reads the 565 swizzle the other way round; hardware wants RGB.
    void setFramebuffer16Bgr(bool bgr);
    Rect viewport() const;

private:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    struct Impl;
    Impl*       m_impl;
    uint32_t*   m_atlas;
    bool        m_stretch;
};

}}

#endif
