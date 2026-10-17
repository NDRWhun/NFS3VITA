#include <platform/gxm.h>
#include <platform/ime.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

// Embedded shader binaries (generated from shaders/*.cg).
extern "C" {
extern const unsigned char nfs3_shader_glide_v[];
extern const unsigned char nfs3_shader_glide_f[];
extern const unsigned char nfs3_shader_blit_v[];
extern const unsigned char nfs3_shader_blit_f[];
extern const unsigned char nfs3_shader_clear_v[];
extern const unsigned char nfs3_shader_clear_f[];
}

namespace pf { namespace gxm {

namespace
{

const uint32_t DisplayBuffers       = 3;
const uint32_t DisplayStride        = DisplayWidth;
const uint32_t PatcherBufferSize    = 64 * 1024;
const uint32_t PatcherUsseSize      = 64 * 1024;
const uint32_t FramebufferBytes     = MaxSourceWidth * MaxSourceHeight * 2;
const uint32_t PaletteBytes         = 256 * 4;
const uint32_t TexturePoolBytes     = 16 * 1024 * 1024;
const uint32_t DialogRefreshMs      = 50;   // longest wait for a game frame while a system dialog is up
const uint32_t GpuDoneGapUs         = 1000; // display callback started this long after the previous one returned
const SceUID   InvalidUid           = -1;

static bool s_framebufferBgr = false; // 565 texture swizzle for Vita3K, see Device::setFramebuffer16Bgr

struct DisplayData
{
    void*       address;
    uint32_t    vblanks;
    uint32_t    frameIndex;     // 0: dialog refresh without a game frame
    uint32_t    endSceneTime;   // perf::now() after the frame's sceGxmEndScene
};

struct QuadVertex
{
    float x, y;
    float u, v;
};

std::atomic<uint32_t> s_completedFrameIndex{0};
std::atomic<uint32_t> s_submittedFrames{0};

uint32_t alignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void* gpuAlloc(SceKernelMemBlockType type, uint32_t size, uint32_t alignment, uint32_t attribs, SceUID* uid)
{
    const uint32_t granularity = (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) ? 256 * 1024 : 4096;
    size = alignUp(size, granularity);
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = alignment;
    *uid = sceKernelAllocMemBlock("nfs3_gpu", type, size, alignment > granularity ? &opt : nullptr);
    if (*uid < 0)
        fatal("sceKernelAllocMemBlock(%u bytes, type 0x%08x) failed: 0x%08x", size, type, *uid);
    void* mem = nullptr;
    sceKernelGetMemBlockBase(*uid, &mem);
    int result = sceGxmMapMemory(mem, size, SceGxmMemoryAttribFlags(attribs));
    if (result < 0)
        fatal("sceGxmMapMemory failed: 0x%08x", result);
    return mem;
}

void gpuFree(SceUID uid)
{
    if (uid < 0)
        return;
    void* mem = nullptr;
    sceKernelGetMemBlockBase(uid, &mem);
    sceGxmUnmapMemory(mem);
    sceKernelFreeMemBlock(uid);
}

void* gpuVertexUsseAlloc(uint32_t size, SceUID* uid, unsigned int* offset)
{
    size = alignUp(size, 4096);
    *uid = sceKernelAllocMemBlock("nfs3_vusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, size, nullptr);
    void* mem = nullptr;
    sceKernelGetMemBlockBase(*uid, &mem);
    sceGxmMapVertexUsseMemory(mem, size, offset);
    return mem;
}

void* gpuFragmentUsseAlloc(uint32_t size, SceUID* uid, unsigned int* offset)
{
    size = alignUp(size, 4096);
    *uid = sceKernelAllocMemBlock("nfs3_fusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, size, nullptr);
    void* mem = nullptr;
    sceKernelGetMemBlockBase(*uid, &mem);
    sceGxmMapFragmentUsseMemory(mem, size, offset);
    return mem;
}

void gpuUsseFree(SceUID uid, bool vertex)
{
    if (uid < 0)
        return;
    void* mem = nullptr;
    sceKernelGetMemBlockBase(uid, &mem);
    if (vertex)
        sceGxmUnmapVertexUsseMemory(mem);
    else
        sceGxmUnmapFragmentUsseMemory(mem);
    sceKernelFreeMemBlock(uid);
}

void* patcherHostAlloc(void*, SceSize size)   { return malloc(size); }
void  patcherHostFree(void*, void* mem)       { free(mem); }

// Display queue thread only: the last two callbacks' end times, the last start, and whether that start was
// the GPU's completion time of its frame.
uint32_t s_lastCallbackStart = 0;
uint32_t s_lastCallbackEnd = 0;
uint32_t s_secondLastCallbackEnd = 0;
bool     s_lastStartWasGpuDone = false;

uint32_t later(uint32_t a, uint32_t b)
{
    return int32_t(a - b) > 0 ? a : b;
}

void displayCallback(const void* data)
{
    const DisplayData* display = static_cast<const DisplayData*>(data);
    const bool perfOn = perf::enabled();
    const bool timed = display->frameIndex && perfOn;
    const uint32_t start = perfOn ? perf::now() : 0;
    // The callback runs once the frame is rendered and the previous callback returned. Starting clearly after
    // that return, it starts at the GPU's completion of the frame.
    const bool startIsGpuDone = perfOn && int32_t(start - s_lastCallbackEnd) > int32_t(GpuDoneGapUs);
    if (timed)
    {
        perf::add(perf::GpuLatency, start - display->endSceneTime);
        if (startIsGpuDone && s_lastStartWasGpuDone)
        {
            // Scenes run in order. This one started at the latest of its submission, the previous scene's
            // completion and the release of its colour buffer (the flip two callbacks back).
            const uint32_t gpuStart = later(later(display->endSceneTime, s_lastCallbackStart), s_secondLastCallbackEnd);
            perf::gpuBusy(start - gpuStart);
        }
    }
    SceDisplayFrameBuf frameBuffer;
    memset(&frameBuffer, 0, sizeof(frameBuffer));
    frameBuffer.size = sizeof(frameBuffer);
    frameBuffer.base = display->address;
    frameBuffer.pitch = DisplayStride;
    frameBuffer.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frameBuffer.width = DisplayWidth;
    frameBuffer.height = DisplayHeight;
    sceDisplaySetFrameBuf(&frameBuffer, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStartMulti(display->vblanks ? display->vblanks : 1);
    if (perfOn)
    {
        const uint32_t end = perf::now();
        if (timed)
            perf::add(perf::FlipWait, end - start);
        s_lastCallbackStart = start;
        s_secondLastCallbackEnd = s_lastCallbackEnd;
        s_lastCallbackEnd = end;
        s_lastStartWasGpuDone = startIsGpuDone;
    }
    if (display->frameIndex)
        s_completedFrameIndex.store(display->frameIndex);
}

SceGxmTextureFormat gxmTextureFormat(TextureFormat format)
{
    switch (format)
    {
    case TextureRgb565:
        return s_framebufferBgr ? SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR : SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB;
    case TextureArgb1555:
        return SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ARGB;
    case TextureArgb4444:
    default:
        return SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_ARGB;
    }
}

// Bits 0, 2, 4, ... of a 16-bit value, packed.
uint32_t evenBits(uint32_t value)
{
    value &= 0x5555;
    value = (value | (value >> 1)) & 0x3333;
    value = (value | (value >> 2)) & 0x0f0f;
    value = (value | (value >> 4)) & 0x00ff;
    return value;
}

// Copies a size² level from rows to the swizzled (Morton) layout: address bits interleave v (bit 0) and u.
void swizzleLevel(uint16_t* out, const uint16_t* in, uint32_t size)
{
    const uint32_t count = size * size;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = in[evenBits(i) * size + evenBits(i >> 1)];
}

unsigned int resourceIndex(const SceGxmProgram* program, const char* name)
{
    const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(program, name);
    if (!parameter)
        fatal("shader parameter %s not found", name);
    return sceGxmProgramParameterGetResourceIndex(parameter);
}

}

struct Texture
{
    SceGxmTexture   gxm;            // swizzled, all levels; filters and addressing are set per draw
    uint32_t        offset;         // in the texture pool
    uint32_t        bytes;
    uint32_t        freedFrame;     // recording frame index when released
    bool            memoryFreed;    // the pool range was reused before the frame completed
    Texture*        next;           // quarantine list
};

struct Device::Impl
{
    // Core
    SceGxmContext*          context = nullptr;
    void*                   hostMem = nullptr;
    SceUID                  vdmRingUid = InvalidUid;
    SceUID                  vertexRingUid = InvalidUid;
    SceUID                  fragmentRingUid = InvalidUid;
    SceUID                  fragmentUsseRingUid = InvalidUid;
    SceGxmRenderTarget*     renderTarget = nullptr;

    // Display
    void*                   displayData[DisplayBuffers] = {};
    SceUID                  displayUid[DisplayBuffers] = { InvalidUid, InvalidUid, InvalidUid };
    SceGxmColorSurface      colorSurface[DisplayBuffers];
    SceGxmSyncObject*       displaySync[DisplayBuffers] = {};
    uint32_t                backBuffer = 0;
    uint32_t                frontBuffer = DisplayBuffers - 1;
    SceUID                  depthUid = InvalidUid;
    SceGxmDepthStencilSurface depthSurface;

    // System dialog: copy of the last presented image without the dialog drawn over it
    void*                   cleanData = nullptr;
    SceUID                  cleanUid = InvalidUid;
    SceGxmColorSurface      cleanSurface;
    bool                    dialogShown = false;    // the front buffer has a dialog drawn over it

    // Shader patcher and programs
    SceGxmShaderPatcher*    patcher = nullptr;
    SceUID                  patcherBufferUid = InvalidUid;
    SceUID                  patcherVertexUsseUid = InvalidUid;
    SceUID                  patcherFragmentUsseUid = InvalidUid;
    SceGxmShaderPatcherId   glideVId = nullptr, glideFId = nullptr;
    SceGxmShaderPatcherId   blitVId = nullptr, blitFId = nullptr;
    SceGxmShaderPatcherId   clearVId = nullptr, clearFId = nullptr;
    SceGxmVertexProgram*    glideV = nullptr;
    SceGxmFragmentProgram*  glideF[2] = {};     // [DrawCall::alphaBlend]
    SceGxmVertexProgram*    blitV = nullptr;
    SceGxmFragmentProgram*  blitF = nullptr;
    SceGxmVertexProgram*    clearV = nullptr;
    SceGxmFragmentProgram*  clearF = nullptr;
    const SceGxmProgramParameter* glideViewportParam = nullptr;
    const SceGxmProgramParameter* clearColorParam = nullptr;
    unsigned int            glideTextureUnit = 0;
    unsigned int            blitTextureUnit = 0;

    // Static geometry: [0..3] full screen quad, [4..7] game viewport quad, [8..11] and [12..15] the bars left and
    // right of the game viewport
    QuadVertex*             quadVertices = nullptr;
    SceUID                  quadVerticesUid = InvalidUid;
    uint16_t*               quadIndices = nullptr;
    SceUID                  quadIndicesUid = InvalidUid;
    uint16_t*               triangleIndices = nullptr;
    SceUID                  triangleIndicesUid = InvalidUid;

    // Texture memory (game thread): first-fit ranges of one CDRAM block. A released texture stays in quarantine
    // until the frames recorded before its release completed.
    struct Range
    {
        uint32_t offset, bytes;
    };
    SceUID                  texturePoolUid = InvalidUid;
    uint8_t*                texturePool = nullptr;
    std::vector<Range>      freeRanges;             // sorted by offset, never adjacent
    Texture*                quarantine = nullptr;

    // Frame slots. A slot is handed back by the render thread once its scene is queued; the game thread then
    // waits for gpuPending before it writes the slot's buffers again.
    struct Slot
    {
        SceUID                      vertexUid = InvalidUid;
        SceUID                      framebufferUid = InvalidUid;
        SceUID                      paletteUid = InvalidUid;
        SceGxmNotification          vertexDone = {};        // vertex stage finished: vertices no longer read
        SceGxmNotification          fragmentDone = {};      // fragment stage finished: framebuffer and palette no longer read
        const SceGxmNotification*   gpuPending = nullptr;   // notification of the slot's last scene, null if none
    };
    Frame                   frames[FrameSlots];
    Slot                    slots[FrameSlots];
    uint32_t                acquireSlot = 0;
    uint32_t                renderSlot = 0;
    bool                    frameAcquired = false;
    uint32_t                nextFrameIndex = 1;
    Semaphore               freeSlots{"nfs3_gxm_free", FrameSlots, FrameSlots};
    Semaphore               readySlots{"nfs3_gxm_ready", 0, FrameSlots + 1}; // +1: shutdown wake-up

    // Render thread
    Thread*                 thread = nullptr;
    volatile bool           running = false;
    Rect                    viewport = { 0, 0, DisplayWidth, DisplayHeight };
    uint32_t                endSceneTime = 0;       // render thread: perf::now() after the last game frame's scene
    bool                    wideShown = false;      // render thread: the last game frame drew outside the viewport

    bool initCore();
    bool initDisplay();
    bool initPrograms();
    bool initBuffers();
    void updateQuads();
    bool allocateRange(uint32_t bytes, uint32_t* offset);
    void freeRange(uint32_t offset, uint32_t bytes);
    void releaseQuarantine(bool force);
    uint32_t recordingFrameIndex() const;

    static int threadEntry(void* self);
    void run();
    void beginScene(SceGxmColorSurface* surface, SceGxmSyncObject* sync);
    void renderFrame(Frame& frame, Slot& slot);
    void setGlideMapping(const Rect& rect, float sourceWidth, float sourceHeight);
    void setGlideTexture(const DrawCall& call);
    void renderDialogFrame();
    void drawQuad(uint32_t first, const SceGxmTexture* texture, SceGxmVertexProgram* vp, SceGxmFragmentProgram* fp, const float* color);
    void drawImage(void* image);
    void* previousImage() const;
    void finishFrame(uint32_t vblanks, uint32_t frameIndex);
    void present(uint32_t vblanks, uint32_t frameIndex);
};

bool Device::Impl::initCore()
{
    SceGxmInitializeParams init;
    memset(&init, 0, sizeof(init));
    init.flags = 0;
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = &displayCallback;
    init.displayQueueCallbackDataSize = sizeof(DisplayData);
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int result = sceGxmInitialize(&init);
    if (result < 0)
    {
        logError("sceGxmInitialize failed: 0x%08x", result);
        return false;
    }

    hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    void* vdmRing = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, 4, SCE_GXM_MEMORY_ATTRIB_READ, &vdmRingUid);
    void* vertexRing = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, 4, SCE_GXM_MEMORY_ATTRIB_READ, &vertexRingUid);
    void* fragmentRing = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, 4, SCE_GXM_MEMORY_ATTRIB_READ, &fragmentRingUid);
    unsigned int fragmentUsseOffset = 0;
    void* fragmentUsseRing = gpuFragmentUsseAlloc(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, &fragmentUsseRingUid, &fragmentUsseOffset);

    SceGxmContextParams params;
    memset(&params, 0, sizeof(params));
    params.hostMem = hostMem;
    params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    params.vdmRingBufferMem = vdmRing;
    params.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    params.vertexRingBufferMem = vertexRing;
    params.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    params.fragmentRingBufferMem = fragmentRing;
    params.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferMem = fragmentUsseRing;
    params.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferOffset = fragmentUsseOffset;
    result = sceGxmCreateContext(&params, &context);
    if (result < 0)
    {
        logError("sceGxmCreateContext failed: 0x%08x", result);
        return false;
    }

    SceGxmRenderTargetParams rtParams;
    memset(&rtParams, 0, sizeof(rtParams));
    rtParams.flags = 0;
    rtParams.width = DisplayWidth;
    rtParams.height = DisplayHeight;
    rtParams.scenesPerFrame = 1;
    rtParams.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rtParams.multisampleLocations = 0;
    rtParams.driverMemBlock = InvalidUid;
    result = sceGxmCreateRenderTarget(&rtParams, &renderTarget);
    if (result < 0)
    {
        logError("sceGxmCreateRenderTarget failed: 0x%08x", result);
        return false;
    }
    return true;
}

bool Device::Impl::initDisplay()
{
    for (uint32_t i = 0; i < DisplayBuffers; ++i)
    {
        displayData[i] = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DisplayStride * DisplayHeight * 4,
                                  SCE_GXM_COLOR_SURFACE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_RW, &displayUid[i]);
        memset(displayData[i], 0, DisplayStride * DisplayHeight * 4);
        int result = sceGxmColorSurfaceInit(&colorSurface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                                            SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                                            DisplayWidth, DisplayHeight, DisplayStride, displayData[i]);
        if (result < 0)
        {
            logError("sceGxmColorSurfaceInit failed: 0x%08x", result);
            return false;
        }
        result = sceGxmSyncObjectCreate(&displaySync[i]);
        if (result < 0)
        {
            logError("sceGxmSyncObjectCreate failed: 0x%08x", result);
            return false;
        }
    }

    cleanData = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DisplayStride * DisplayHeight * 4,
                         SCE_GXM_COLOR_SURFACE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_RW, &cleanUid);
    int result = sceGxmColorSurfaceInit(&cleanSurface, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                                        SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                                        DisplayWidth, DisplayHeight, DisplayStride, cleanData);
    if (result < 0)
    {
        logError("sceGxmColorSurfaceInit (dialog copy) failed: 0x%08x", result);
        return false;
    }

    const uint32_t depthStride = alignUp(DisplayWidth, SCE_GXM_TILE_SIZEX);
    const uint32_t depthHeight = alignUp(DisplayHeight, SCE_GXM_TILE_SIZEY);
    void* depthData = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, depthStride * depthHeight * 4,
                               SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_RW, &depthUid);
    result = sceGxmDepthStencilSurfaceInit(&depthSurface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32,
                                           SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, depthStride, depthData, nullptr);
    if (result < 0)
    {
        logError("sceGxmDepthStencilSurfaceInit failed: 0x%08x", result);
        return false;
    }
    return true;
}

bool Device::Impl::initPrograms()
{
    void* patcherBuffer = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, PatcherBufferSize, 4, SCE_GXM_MEMORY_ATTRIB_RW, &patcherBufferUid);
    unsigned int vertexUsseOffset = 0, fragmentUsseOffset = 0;
    void* vertexUsse = gpuVertexUsseAlloc(PatcherUsseSize, &patcherVertexUsseUid, &vertexUsseOffset);
    void* fragmentUsse = gpuFragmentUsseAlloc(PatcherUsseSize, &patcherFragmentUsseUid, &fragmentUsseOffset);

    SceGxmShaderPatcherParams params;
    memset(&params, 0, sizeof(params));
    params.userData = nullptr;
    params.hostAllocCallback = &patcherHostAlloc;
    params.hostFreeCallback = &patcherHostFree;
    params.bufferAllocCallback = nullptr;
    params.bufferFreeCallback = nullptr;
    params.bufferMem = patcherBuffer;
    params.bufferMemSize = PatcherBufferSize;
    params.vertexUsseAllocCallback = nullptr;
    params.vertexUsseFreeCallback = nullptr;
    params.vertexUsseMem = vertexUsse;
    params.vertexUsseMemSize = PatcherUsseSize;
    params.vertexUsseOffset = vertexUsseOffset;
    params.fragmentUsseAllocCallback = nullptr;
    params.fragmentUsseFreeCallback = nullptr;
    params.fragmentUsseMem = fragmentUsse;
    params.fragmentUsseMemSize = PatcherUsseSize;
    params.fragmentUsseOffset = fragmentUsseOffset;
    int result = sceGxmShaderPatcherCreate(&params, &patcher);
    if (result < 0)
    {
        logError("sceGxmShaderPatcherCreate failed: 0x%08x", result);
        return false;
    }

    struct ProgramEntry { const unsigned char* data; SceGxmShaderPatcherId* id; const char* name; };
    ProgramEntry entries[] = {
        { nfs3_shader_glide_v, &glideVId, "glide_v" },
        { nfs3_shader_glide_f, &glideFId, "glide_f" },
        { nfs3_shader_blit_v,  &blitVId,  "blit_v" },  { nfs3_shader_blit_f,  &blitFId,  "blit_f" },
        { nfs3_shader_clear_v, &clearVId, "clear_v" }, { nfs3_shader_clear_f, &clearFId, "clear_f" },
    };
    for (const ProgramEntry& entry : entries)
    {
        const SceGxmProgram* program = reinterpret_cast<const SceGxmProgram*>(entry.data);
        result = sceGxmProgramCheck(program);
        if (result < 0)
        {
            logError("shader %s is invalid: 0x%08x", entry.name, result);
            return false;
        }
        result = sceGxmShaderPatcherRegisterProgram(patcher, program, entry.id);
        if (result < 0)
        {
            logError("registering shader %s failed: 0x%08x", entry.name, result);
            return false;
        }
    }

    const SceGxmProgram* glideVProgram = reinterpret_cast<const SceGxmProgram*>(nfs3_shader_glide_v);
    const SceGxmProgram* glideFProgram = reinterpret_cast<const SceGxmProgram*>(nfs3_shader_glide_f);
    const SceGxmProgram* blitVProgram = reinterpret_cast<const SceGxmProgram*>(nfs3_shader_blit_v);
    const SceGxmProgram* blitFProgram = reinterpret_cast<const SceGxmProgram*>(nfs3_shader_blit_f);
    const SceGxmProgram* clearVProgram = reinterpret_cast<const SceGxmProgram*>(nfs3_shader_clear_v);
    const SceGxmProgram* clearFProgram = reinterpret_cast<const SceGxmProgram*>(nfs3_shader_clear_f);

    // Glide vertex layout mirrors pf::gxm::Vertex.
    SceGxmVertexAttribute glideAttributes[4];
    glideAttributes[0] = { 0, 0,  SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, uint16_t(resourceIndex(glideVProgram, "aPosition")) };
    glideAttributes[1] = { 0, 12, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, uint16_t(resourceIndex(glideVProgram, "aColor")) };
    glideAttributes[2] = { 0, 16, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, uint16_t(resourceIndex(glideVProgram, "aTexCoord")) };
    glideAttributes[3] = { 0, 24, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, uint16_t(resourceIndex(glideVProgram, "aCombine")) };
    SceGxmVertexStream glideStream = { sizeof(Vertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    result = sceGxmShaderPatcherCreateVertexProgram(patcher, glideVId, glideAttributes, 4, &glideStream, 1, &glideV);
    if (result < 0)
    {
        logError("glide vertex program creation failed: 0x%08x", result);
        return false;
    }

    SceGxmBlendInfo blend;
    memset(&blend, 0, sizeof(blend));
    blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
    blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    for (int alphaBlend = 0; alphaBlend < 2; ++alphaBlend)
    {
        blend.colorDst = alphaBlend ? SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : SCE_GXM_BLEND_FACTOR_ONE;
        result = sceGxmShaderPatcherCreateFragmentProgram(patcher, glideFId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                          SCE_GXM_MULTISAMPLE_NONE, &blend, glideVProgram, &glideF[alphaBlend]);
        if (result < 0)
        {
            logError("glide fragment program creation failed (blend %d): 0x%08x", alphaBlend, result);
            return false;
        }
    }
    glideViewportParam = sceGxmProgramFindParameterByName(glideVProgram, "uViewport");
    glideTextureUnit = resourceIndex(glideFProgram, "uTexture");

    SceGxmVertexAttribute quadAttributes[2];
    quadAttributes[0] = { 0, 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, uint16_t(resourceIndex(blitVProgram, "aPosition")) };
    quadAttributes[1] = { 0, 8, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, uint16_t(resourceIndex(blitVProgram, "aTexCoord")) };
    SceGxmVertexStream quadStream = { sizeof(QuadVertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    result = sceGxmShaderPatcherCreateVertexProgram(patcher, blitVId, quadAttributes, 2, &quadStream, 1, &blitV);
    if (result < 0)
    {
        logError("blit vertex program creation failed: 0x%08x", result);
        return false;
    }
    result = sceGxmShaderPatcherCreateFragmentProgram(patcher, blitFId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                      SCE_GXM_MULTISAMPLE_NONE, nullptr, blitVProgram, &blitF);
    if (result < 0)
    {
        logError("blit fragment program creation failed: 0x%08x", result);
        return false;
    }
    blitTextureUnit = resourceIndex(blitFProgram, "uTexture");

    SceGxmVertexAttribute clearAttribute = { 0, 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, uint16_t(resourceIndex(clearVProgram, "aPosition")) };
    result = sceGxmShaderPatcherCreateVertexProgram(patcher, clearVId, &clearAttribute, 1, &quadStream, 1, &clearV);
    if (result < 0)
    {
        logError("clear vertex program creation failed: 0x%08x", result);
        return false;
    }
    result = sceGxmShaderPatcherCreateFragmentProgram(patcher, clearFId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                      SCE_GXM_MULTISAMPLE_NONE, nullptr, clearVProgram, &clearF);
    if (result < 0)
    {
        logError("clear fragment program creation failed: 0x%08x", result);
        return false;
    }
    clearColorParam = sceGxmProgramFindParameterByName(clearFProgram, "uColor");
    if (!glideViewportParam || !clearColorParam)
    {
        logError("shader uniforms not found");
        return false;
    }
    return true;
}

bool Device::Impl::initBuffers()
{
    quadVertices = static_cast<QuadVertex*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, sizeof(QuadVertex) * 16, 4,
                                                     SCE_GXM_MEMORY_ATTRIB_READ, &quadVerticesUid));
    quadIndices = static_cast<uint16_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, sizeof(uint16_t) * 4, 4,
                                                  SCE_GXM_MEMORY_ATTRIB_READ, &quadIndicesUid));
    for (uint16_t i = 0; i < 4; ++i)
        quadIndices[i] = i;
    triangleIndices = static_cast<uint16_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, sizeof(uint16_t) * MaxVertices, 4,
                                                      SCE_GXM_MEMORY_ATTRIB_READ, &triangleIndicesUid));
    for (uint32_t i = 0; i < MaxVertices; ++i)
        triangleIndices[i] = uint16_t(i);
    updateQuads();

    // Two notification words per slot; frame indices start at 1, so a zeroed word never matches.
    volatile unsigned int* notifications = sceGxmGetNotificationRegion();
    for (uint32_t i = 0; i < FrameSlots; ++i)
    {
        Frame& frame = frames[i];
        Slot& slot = slots[i];
        frame.vertices = static_cast<Vertex*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, sizeof(Vertex) * MaxVertices, 4,
                                                       SCE_GXM_MEMORY_ATTRIB_READ, &slot.vertexUid));
        frame.framebuffer = static_cast<uint8_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, FramebufferBytes,
                                                           SCE_GXM_TEXTURE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_READ, &slot.framebufferUid));
        frame.palette = static_cast<uint32_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, PaletteBytes,
                                                        SCE_GXM_PALETTE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_READ, &slot.paletteUid));
        frame.drawCalls.reserve(256);
        slot.vertexDone.address = notifications + 2 * i;
        slot.fragmentDone.address = notifications + 2 * i + 1;
        *slot.vertexDone.address = 0;
        *slot.fragmentDone.address = 0;
    }
    return true;
}

// Rebuilds the static quads for the current viewport (NDC, v = 0 at the top).
void Device::Impl::updateQuads()
{
    const float fx0 = -1.0f, fx1 = 1.0f, fy0 = 1.0f, fy1 = -1.0f;
    quadVertices[0] = { fx0, fy0, 0.0f, 0.0f };
    quadVertices[1] = { fx1, fy0, 1.0f, 0.0f };
    quadVertices[2] = { fx0, fy1, 0.0f, 1.0f };
    quadVertices[3] = { fx1, fy1, 1.0f, 1.0f };

    const float x0 = float(viewport.x) * 2.0f / DisplayWidth - 1.0f;
    const float x1 = float(viewport.x + viewport.w) * 2.0f / DisplayWidth - 1.0f;
    const float y0 = 1.0f - float(viewport.y) * 2.0f / DisplayHeight;
    const float y1 = 1.0f - float(viewport.y + viewport.h) * 2.0f / DisplayHeight;
    quadVertices[4] = { x0, y0, 0.0f, 0.0f };
    quadVertices[5] = { x1, y0, 1.0f, 0.0f };
    quadVertices[6] = { x0, y1, 0.0f, 1.0f };
    quadVertices[7] = { x1, y1, 1.0f, 1.0f };

    quadVertices[8] = { fx0, fy0, 0.0f, 0.0f };
    quadVertices[9] = { x0, fy0, 0.0f, 0.0f };
    quadVertices[10] = { fx0, fy1, 0.0f, 0.0f };
    quadVertices[11] = { x0, fy1, 0.0f, 0.0f };
    quadVertices[12] = { x1, fy0, 0.0f, 0.0f };
    quadVertices[13] = { fx1, fy0, 0.0f, 0.0f };
    quadVertices[14] = { x1, fy1, 0.0f, 0.0f };
    quadVertices[15] = { fx1, fy1, 0.0f, 0.0f };
}

int Device::Impl::threadEntry(void* self)
{
    static_cast<Impl*>(self)->run();
    return 0;
}

void Device::Impl::run()
{
    while (true)
    {
        if (ime::pending())
        {
            // A system dialog needs frames even while the game submits none.
            if (!readySlots.waitFor(DialogRefreshMs))
            {
                if (!running)
                    break;
                renderDialogFrame();
                continue;
            }
        }
        else
        {
            readySlots.wait();
        }
        if (!running)
            break;
        Frame& frame = frames[renderSlot];
        const bool timed = perf::enabled() && frame.kind != FrameEmpty;
        const uint32_t start = timed ? perf::now() : 0;
        const uint32_t drawCalls = uint32_t(frame.drawCalls.size());
        const uint32_t triangles = frame.vertexCount / 3;
        renderFrame(frame, slots[renderSlot]);
        const uint32_t end = timed ? perf::now() : 0;
        renderSlot = (renderSlot + 1) % FrameSlots;
        freeSlots.post();
        if (timed)
        {
            perf::add(perf::Render, end - start);
            perf::framePresented(drawCalls, triangles);
        }
    }
}

void Device::Impl::beginScene(SceGxmColorSurface* surface, SceGxmSyncObject* sync)
{
    sceGxmBeginScene(context, 0, renderTarget, nullptr, nullptr, sync, surface, &depthSurface);
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetViewport(context, 0.5f * DisplayWidth, 0.5f * DisplayWidth, 0.5f * DisplayHeight, -0.5f * DisplayHeight, 0.0f, 1.0f);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, DisplayWidth - 1, DisplayHeight - 1);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
}

void Device::Impl::drawQuad(uint32_t first, const SceGxmTexture* texture, SceGxmVertexProgram* vp, SceGxmFragmentProgram* fp, const float* color)
{
    sceGxmSetVertexProgram(context, vp);
    sceGxmSetFragmentProgram(context, fp);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    if (texture)
    {
        sceGxmSetFragmentTexture(context, blitTextureUnit, texture);
    }
    if (color)
    {
        void* uniforms = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, clearColorParam, 0, 4, color);
    }
    sceGxmSetVertexStream(context, 0, quadVertices + first);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, quadIndices, 4);
}

// Copies a display-sized ABGR image over the whole target.
void Device::Impl::drawImage(void* image)
{
    SceGxmTexture texture;
    sceGxmTextureInitLinear(&texture, image, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, DisplayWidth, DisplayHeight, 1);
    sceGxmTextureSetMinFilter(&texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&texture, SCE_GXM_TEXTURE_FILTER_POINT);
    drawQuad(0, &texture, blitV, blitF, nullptr);
}

// The last presented image without a system dialog over it.
void* Device::Impl::previousImage() const
{
    return dialogShown ? cleanData : displayData[frontBuffer];
}

void Device::Impl::renderFrame(Frame& frame, Slot& slot)
{
    if (frame.kind == FrameEmpty)
    {
        // Nothing read from the slot; the next displayed frame also completes this index.
        if (ime::pending())
            renderDialogFrame();
        return;
    }

    beginScene(&colorSurface[backBuffer], displaySync[backBuffer]);

    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (frame.kind == FrameFramebuffer16 || frame.kind == FrameFramebuffer8)
    {
        SceGxmTexture texture;
        SceGxmTextureFormat format = SCE_GXM_TEXTURE_FORMAT_P8_ABGR;
        if (frame.kind == FrameFramebuffer16)
            format = s_framebufferBgr ? SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR : SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB;
        sceGxmTextureInitLinear(&texture, frame.framebuffer, format, frame.sourceWidth, frame.sourceHeight, 1);
        if (frame.kind == FrameFramebuffer8)
            sceGxmTextureSetPalette(&texture, frame.palette);
        sceGxmTextureSetMinFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetUAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
        drawQuad(0, nullptr, clearV, clearF, black);
        drawQuad(4, &texture, blitV, blitF, nullptr);
        wideShown = false;
    }
    else
    {
        if (frame.clear)
        {
            const float color[4] = {
                float((frame.clearColor >> 16) & 0xff) / 255.0f,
                float((frame.clearColor >> 8) & 0xff) / 255.0f,
                float(frame.clearColor & 0xff) / 255.0f,
                1.0f };
            if (frame.wide)
            {
                drawQuad(0, nullptr, clearV, clearF, color);
            }
            else
            {
                drawQuad(0, nullptr, clearV, clearF, black);
                drawQuad(4, nullptr, clearV, clearF, color);
            }
        }
        else
        {
            // No clear: keep drawing over the previously displayed frame, without world pixels this frame
            // does not cover beside the game area.
            drawImage(previousImage());
            if (wideShown && !frame.wide)
            {
                drawQuad(8, nullptr, clearV, clearF, black);
                drawQuad(12, nullptr, clearV, clearF, black);
            }
        }
        wideShown = frame.wide;

        if (frame.vertexCount)
        {
            const float sw = float(frame.sourceWidth ? frame.sourceWidth : 640);
            const float sh = float(frame.sourceHeight ? frame.sourceHeight : 480);
            const Rect full = { 0, 0, DisplayWidth, DisplayHeight };

            sceGxmSetVertexProgram(context, glideV);
            sceGxmSetVertexStream(context, 0, frame.vertices);
            Mapping mapping = frame.drawCalls.empty() ? MapBox : frame.drawCalls.front().mapping;
            setGlideMapping(mapping == MapFull ? full : viewport, sw, sh);

            uint32_t first = 0;
            for (const DrawCall& call : frame.drawCalls)
            {
                uint32_t count = call.lastVertex - first;
                if (count)
                {
                    if (call.mapping != mapping)
                    {
                        mapping = call.mapping;
                        setGlideMapping(mapping == MapFull ? full : viewport, sw, sh);
                    }
                    setGlideTexture(call);
                    sceGxmSetFrontDepthFunc(context, call.zTest ? SCE_GXM_DEPTH_FUNC_LESS_EQUAL : SCE_GXM_DEPTH_FUNC_ALWAYS);
                    sceGxmSetFrontDepthWriteEnable(context, call.zWrite ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
                    sceGxmSetFragmentProgram(context, glideF[call.alphaBlend]);
                    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, triangleIndices + first, count);
                }
                first = call.lastVertex;
            }
            sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, DisplayWidth - 1, DisplayHeight - 1);
        }
    }

    // Glide frames only read the slot's vertices (vertex stage); framebuffer frames read its texture and palette.
    slot.vertexDone.value = frame.frameIndex;
    slot.fragmentDone.value = frame.frameIndex;
    sceGxmEndScene(context, &slot.vertexDone, &slot.fragmentDone);
    slot.gpuPending = frame.kind == FrameGlide ? &slot.vertexDone : &slot.fragmentDone;
    if (perf::enabled())
        endSceneTime = perf::now();
    finishFrame(frame.vblanks, frame.frameIndex);
}

// Maps Glide screen coordinates (0..sourceWidth, 0..sourceHeight) onto a display rectangle for the following
// draws: a new default uniform buffer for glide_v and a region clip (tile granular; the game clips polygons itself).
void Device::Impl::setGlideMapping(const Rect& rect, float sourceWidth, float sourceHeight)
{
    const float viewportUniform[4] = {
        2.0f * float(rect.w) / (sourceWidth * DisplayWidth),
        -2.0f * float(rect.h) / (sourceHeight * DisplayHeight),
        2.0f * float(rect.x) / DisplayWidth - 1.0f,
        1.0f - 2.0f * float(rect.y) / DisplayHeight };
    void* uniforms = nullptr;
    sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
    sceGxmSetUniformDataF(uniforms, glideViewportParam, 0, 4, viewportUniform);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, rect.x, rect.y, rect.x + rect.w - 1, rect.y + rect.h - 1);
}

// Binds the call's texture with its addressing, filtering and mipmap use.
void Device::Impl::setGlideTexture(const DrawCall& call)
{
    SceGxmTexture texture = call.texture->gxm;
    const SceGxmTextureFilter filter = call.texPoint ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
    sceGxmTextureSetMinFilter(&texture, filter);
    sceGxmTextureSetMagFilter(&texture, filter);
    const SceGxmTextureAddrMode addrMode = call.texClamp ? SCE_GXM_TEXTURE_ADDR_CLAMP : SCE_GXM_TEXTURE_ADDR_REPEAT;
    sceGxmTextureSetUAddrMode(&texture, addrMode);
    sceGxmTextureSetVAddrMode(&texture, addrMode);
    if (call.mipMode == MipNone)
        sceGxmTextureSetMipmapCount(&texture, 1);
    sceGxmTextureSetMipFilter(&texture, call.mipMode == MipBlend ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
                                                                 : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    sceGxmSetFragmentTexture(context, glideTextureUnit, &texture);
}

// Re-presents the last image so a system dialog keeps updating while no game frame arrives.
void Device::Impl::renderDialogFrame()
{
    beginScene(&colorSurface[backBuffer], displaySync[backBuffer]);
    drawImage(previousImage());
    sceGxmEndScene(context, nullptr, nullptr);
    finishFrame(1, 0);
}

// Draws a running system dialog over the finished scene, then queues the back buffer for display.
void Device::Impl::finishFrame(uint32_t vblanks, uint32_t frameIndex)
{
    dialogShown = ime::prepareFrame();
    if (dialogShown)
    {
        // Dialog-free copy for the next frame that draws over the previous image. The back buffer's sync
        // object makes the dialog draw after this copy has read the buffer.
        beginScene(&cleanSurface, displaySync[backBuffer]);
        drawImage(displayData[backBuffer]);
        sceGxmEndScene(context, nullptr, nullptr);
        ime::drawFrame(displayData[backBuffer], DisplayWidth, DisplayHeight, DisplayStride, displaySync[backBuffer]);
    }
    present(vblanks, frameIndex);
}

void Device::Impl::present(uint32_t vblanks, uint32_t frameIndex)
{
    sceGxmPadHeartbeat(&colorSurface[backBuffer], displaySync[backBuffer]);
    DisplayData data;
    data.address = displayData[backBuffer];
    data.vblanks = vblanks;
    data.frameIndex = frameIndex;
    data.endSceneTime = endSceneTime;
    const bool timed = frameIndex && perf::enabled();
    const uint32_t start = timed ? perf::now() : 0;
    sceGxmDisplayQueueAddEntry(displaySync[frontBuffer], displaySync[backBuffer], &data);
    if (timed)
        perf::add(perf::QueueWait, perf::now() - start);
    frontBuffer = backBuffer;
    backBuffer = (backBuffer + 1) % DisplayBuffers;
}

Device& Device::get()
{
    static Device device;
    return device;
}

Device::Device()
    :   m_impl(nullptr)
    ,   m_stretch(false)
{
}

Device::~Device()
{
}

bool Device::init()
{
    if (m_impl)
        return true;
    m_impl = new Impl();
    m_impl->viewport = viewport();
    if (!m_impl->initCore() || !m_impl->initDisplay() || !m_impl->initPrograms() || !m_impl->initBuffers())
        return false;

    m_impl->texturePool = static_cast<uint8_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, TexturePoolBytes,
                                                         SCE_GXM_TEXTURE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_READ,
                                                         &m_impl->texturePoolUid));
    m_impl->freeRanges.push_back(Impl::Range{ 0, TexturePoolBytes });

    m_impl->running = true;
    m_impl->thread = new Thread("nfs3_render", &Impl::threadEntry, m_impl, Thread::PriorityHigh, 256 * 1024, Thread::CpuAll);
    m_impl->thread->start();
    logInfo("gxm device ready: %dx%d, viewport %d,%d %dx%d", DisplayWidth, DisplayHeight,
            m_impl->viewport.x, m_impl->viewport.y, m_impl->viewport.w, m_impl->viewport.h);
    return true;
}

void Device::shutdown()
{
    if (!m_impl)
        return;
    Impl& impl = *m_impl;
    if (impl.thread)
    {
        impl.running = false;
        impl.readySlots.post();
        impl.thread->join();
        delete impl.thread;
        impl.thread = nullptr;
    }
    ime::shutdown();
    if (impl.context)
    {
        sceGxmFinish(impl.context);
        sceGxmDisplayQueueFinish();
    }
    if (impl.patcher)
    {
        if (impl.glideV) sceGxmShaderPatcherReleaseVertexProgram(impl.patcher, impl.glideV);
        for (SceGxmFragmentProgram* program : impl.glideF)
            if (program) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, program);
        if (impl.blitV) sceGxmShaderPatcherReleaseVertexProgram(impl.patcher, impl.blitV);
        if (impl.blitF) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, impl.blitF);
        if (impl.clearV) sceGxmShaderPatcherReleaseVertexProgram(impl.patcher, impl.clearV);
        if (impl.clearF) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, impl.clearF);
        SceGxmShaderPatcherId ids[] = { impl.glideVId, impl.glideFId, impl.blitVId, impl.blitFId,
                                        impl.clearVId, impl.clearFId };
        for (SceGxmShaderPatcherId id : ids)
            if (id) sceGxmShaderPatcherUnregisterProgram(impl.patcher, id);
        sceGxmShaderPatcherDestroy(impl.patcher);
    }
    gpuUsseFree(impl.patcherVertexUsseUid, true);
    gpuUsseFree(impl.patcherFragmentUsseUid, false);
    gpuFree(impl.patcherBufferUid);
    for (Impl::Slot& slot : impl.slots)
    {
        gpuFree(slot.vertexUid);
        gpuFree(slot.framebufferUid);
        gpuFree(slot.paletteUid);
    }
    while (Texture* texture = impl.quarantine)
    {
        impl.quarantine = texture->next;
        delete texture;
    }
    gpuFree(impl.texturePoolUid);
    gpuFree(impl.quadVerticesUid);
    gpuFree(impl.quadIndicesUid);
    gpuFree(impl.triangleIndicesUid);
    for (uint32_t i = 0; i < DisplayBuffers; ++i)
    {
        if (impl.displaySync[i]) sceGxmSyncObjectDestroy(impl.displaySync[i]);
        gpuFree(impl.displayUid[i]);
    }
    gpuFree(impl.cleanUid);
    gpuFree(impl.depthUid);
    if (impl.renderTarget) sceGxmDestroyRenderTarget(impl.renderTarget);
    if (impl.context) sceGxmDestroyContext(impl.context);
    gpuUsseFree(impl.fragmentUsseRingUid, false);
    gpuFree(impl.fragmentRingUid);
    gpuFree(impl.vertexRingUid);
    gpuFree(impl.vdmRingUid);
    free(impl.hostMem);
    sceGxmTerminate();
    delete m_impl;
    m_impl = nullptr;
}

Frame& Device::acquireFrame()
{
    Impl& impl = *m_impl;
    if (!impl.frameAcquired)
    {
        const bool timed = perf::enabled();
        const uint32_t start = timed ? perf::now() : 0;
        impl.freeSlots.wait();
        Impl::Slot& slot = impl.slots[impl.acquireSlot];
        if (slot.gpuPending)
        {
            const uint32_t fenceStart = timed ? perf::now() : 0;
            int result = sceGxmNotificationWait(slot.gpuPending);
            if (result < 0)
                logError("sceGxmNotificationWait failed: 0x%08x", result);
            slot.gpuPending = nullptr;
            if (timed)
                perf::add(perf::FenceWait, perf::now() - fenceStart);
        }
        if (timed)
            perf::add(perf::SlotWait, perf::now() - start);
        Frame& frame = impl.frames[impl.acquireSlot];
        frame.kind = FrameEmpty;
        frame.vblanks = 1;
        frame.frameIndex = impl.nextFrameIndex++;
        frame.sourceWidth = 0;
        frame.sourceHeight = 0;
        frame.clear = false;
        frame.clearColor = 0;
        frame.wide = false;
        frame.vertexCount = 0;
        frame.drawCalls.clear();
        impl.frameAcquired = true;
    }
    return impl.frames[impl.acquireSlot];
}

Frame* Device::currentFrame()
{
    Impl& impl = *m_impl;
    return impl.frameAcquired ? &impl.frames[impl.acquireSlot] : nullptr;
}

void Device::submitFrame()
{
    Impl& impl = *m_impl;
    if (!impl.frameAcquired)
        return;
    impl.frameAcquired = false;
    impl.acquireSlot = (impl.acquireSlot + 1) % FrameSlots;
    s_submittedFrames.fetch_add(1);
    impl.readySlots.post();
}

uint32_t Device::submittedFrameCount() const
{
    return s_submittedFrames.load();
}

void Device::setFramebuffer16Bgr(bool bgr)
{
    s_framebufferBgr = bgr;
}

uint32_t Device::Impl::recordingFrameIndex() const
{
    return frameAcquired ? frames[acquireSlot].frameIndex : nextFrameIndex;
}

bool Device::Impl::allocateRange(uint32_t bytes, uint32_t* offset)
{
    for (size_t i = 0; i < freeRanges.size(); ++i)
    {
        Range& range = freeRanges[i];
        if (range.bytes < bytes)
            continue;
        *offset = range.offset;
        range.offset += bytes;
        range.bytes -= bytes;
        if (!range.bytes)
            freeRanges.erase(freeRanges.begin() + i);
        return true;
    }
    return false;
}

void Device::Impl::freeRange(uint32_t offset, uint32_t bytes)
{
    size_t i = 0;
    while (i < freeRanges.size() && freeRanges[i].offset < offset)
        ++i;
    const bool joinsPrevious = i > 0 && freeRanges[i - 1].offset + freeRanges[i - 1].bytes == offset;
    const bool joinsNext = i < freeRanges.size() && offset + bytes == freeRanges[i].offset;
    if (joinsPrevious && joinsNext)
    {
        freeRanges[i - 1].bytes += bytes + freeRanges[i].bytes;
        freeRanges.erase(freeRanges.begin() + i);
    }
    else if (joinsPrevious)
    {
        freeRanges[i - 1].bytes += bytes;
    }
    else if (joinsNext)
    {
        freeRanges[i].offset = offset;
        freeRanges[i].bytes += bytes;
    }
    else
    {
        freeRanges.insert(freeRanges.begin() + i, Range{ offset, bytes });
    }
}

// Frees quarantined textures whose frames completed. With force, also hands out the memory of the others; their
// objects stay until their frames completed, because recorded draw calls still point to them.
void Device::Impl::releaseQuarantine(bool force)
{
    const uint32_t completed = s_completedFrameIndex.load();
    Texture** link = &quarantine;
    while (Texture* texture = *link)
    {
        const bool done = int32_t(completed - texture->freedFrame) >= 0;
        if ((done || force) && !texture->memoryFreed)
        {
            freeRange(texture->offset, texture->bytes);
            texture->memoryFreed = true;
        }
        if (done)
        {
            *link = texture->next;
            delete texture;
        }
        else
        {
            link = &texture->next;
        }
    }
}

Texture* Device::createTexture(TextureFormat format, uint32_t size, uint32_t levels, const uint16_t* texels)
{
    Impl& impl = *m_impl;
    uint32_t texelCount = 0;
    for (uint32_t level = 0; level < levels; ++level)
        texelCount += (size >> level) * (size >> level);
    const uint32_t bytes = alignUp(texelCount * 2, SCE_GXM_TEXTURE_ALIGNMENT);
    uint32_t offset = 0;
    impl.releaseQuarantine(false);
    if (!impl.allocateRange(bytes, &offset))
    {
        logError("texture memory full: reusing released textures that frames in flight may still read");
        impl.releaseQuarantine(true);
        if (!impl.allocateRange(bytes, &offset))
            fatal("texture memory exhausted (%u bytes requested)", bytes);
    }

    uint16_t* data = reinterpret_cast<uint16_t*>(impl.texturePool + offset);
    uint16_t* out = data;
    for (uint32_t level = 0; level < levels; ++level)
    {
        const uint32_t levelSize = size >> level;
        swizzleLevel(out, texels, levelSize);
        out += levelSize * levelSize;
        texels += levelSize * levelSize;
    }

    Texture* texture = new Texture();
    texture->offset = offset;
    texture->bytes = bytes;
    texture->freedFrame = 0;
    texture->memoryFreed = false;
    texture->next = nullptr;
    const int result = sceGxmTextureInitSwizzled(&texture->gxm, data, gxmTextureFormat(format), size, size, levels);
    if (result < 0)
        fatal("sceGxmTextureInitSwizzled(%u, %u levels) failed: 0x%08x", size, levels, result);
    return texture;
}

void Device::releaseTexture(Texture* texture)
{
    Impl& impl = *m_impl;
    texture->freedFrame = impl.recordingFrameIndex();
    texture->next = impl.quarantine;
    impl.quarantine = texture;
}

Rect Device::viewport() const
{
    if (m_stretch)
    {
        Rect full = { 0, 0, DisplayWidth, DisplayHeight };
        return full;
    }
    // 4:3 game area centered on the 16:9 screen, width rounded down to a multiple of 4.
    int width = (DisplayHeight * 4 / 3) & ~3;
    Rect rect = { (DisplayWidth - width) / 2, 0, width, DisplayHeight };
    return rect;
}

}}
