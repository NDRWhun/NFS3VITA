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
    MaxSourceWidth  = 800,
    MaxSourceHeight = 600,
    FrameSlots      = 2
};

// Glide triangle vertex as consumed by glide_v.cg (56 bytes).
struct Vertex
{
    float       x, y, z;
    uint32_t    color;
    float       u, v;
    float       colorCombine, alphaCombine, texWrap, oow;
    float       atlasSize, width, offX, offY;
};

struct DrawCall
{
    uint32_t    lastVertex;
    bool        zTest;
    bool        zWrite;
    bool        alphaBlend;
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
    Frame& acquireFrame();
    Frame* currentFrame();
    void submitFrame();

    uint32_t* atlas() const { return m_atlas; }
    uint32_t completedFrameIndex() const;
    uint32_t recordingFrameIndex() const;

    void setStretch(bool stretch) { m_stretch = stretch; }
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
