#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

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
const SceUID   InvalidUid           = -1;

struct DisplayData
{
    void*       address;
    uint32_t    vblanks;
    uint32_t    frameIndex;
};

struct QuadVertex
{
    float x, y;
    float u, v;
};

std::atomic<uint32_t> s_completedFrameIndex{0};

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

void displayCallback(const void* data)
{
    const DisplayData* display = static_cast<const DisplayData*>(data);
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
    s_completedFrameIndex.store(display->frameIndex);
}

unsigned int resourceIndex(const SceGxmProgram* program, const char* name)
{
    const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(program, name);
    if (!parameter)
        fatal("shader parameter %s not found", name);
    return sceGxmProgramParameterGetResourceIndex(parameter);
}

}

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

    // Shader patcher and programs
    SceGxmShaderPatcher*    patcher = nullptr;
    SceUID                  patcherBufferUid = InvalidUid;
    SceUID                  patcherVertexUsseUid = InvalidUid;
    SceUID                  patcherFragmentUsseUid = InvalidUid;
    SceGxmShaderPatcherId   glideVId = nullptr, glideFId = nullptr;
    SceGxmShaderPatcherId   blitVId = nullptr, blitFId = nullptr;
    SceGxmShaderPatcherId   clearVId = nullptr, clearFId = nullptr;
    SceGxmVertexProgram*    glideV = nullptr;
    SceGxmFragmentProgram*  glideFBlend = nullptr;
    SceGxmFragmentProgram*  glideFAdd = nullptr;
    SceGxmVertexProgram*    blitV = nullptr;
    SceGxmFragmentProgram*  blitF = nullptr;
    SceGxmVertexProgram*    clearV = nullptr;
    SceGxmFragmentProgram*  clearF = nullptr;
    const SceGxmProgramParameter* glideViewportParam = nullptr;
    const SceGxmProgramParameter* clearColorParam = nullptr;
    unsigned int            glideTextureUnit = 0;
    unsigned int            blitTextureUnit = 0;

    // Static geometry: [0..3] full screen quad, [4..7] game viewport quad
    QuadVertex*             quadVertices = nullptr;
    SceUID                  quadVerticesUid = InvalidUid;
    uint16_t*               quadIndices = nullptr;
    SceUID                  quadIndicesUid = InvalidUid;
    uint16_t*               triangleIndices = nullptr;
    SceUID                  triangleIndicesUid = InvalidUid;

    // Atlas
    SceUID                  atlasUid = InvalidUid;
    SceGxmTexture           atlasTexture;

    // Frame slots
    Frame                   frames[FrameSlots];
    SceUID                  frameVertexUid[FrameSlots] = { InvalidUid, InvalidUid };
    SceUID                  frameFramebufferUid[FrameSlots] = { InvalidUid, InvalidUid };
    SceUID                  framePaletteUid[FrameSlots] = { InvalidUid, InvalidUid };
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

    bool initCore();
    bool initDisplay();
    bool initPrograms();
    bool initBuffers();
    void updateQuads();

    static int threadEntry(void* self);
    void run();
    void renderFrame(Frame& frame);
    void drawQuad(uint32_t first, const SceGxmTexture* texture, SceGxmVertexProgram* vp, SceGxmFragmentProgram* fp, const float* color);
    void present(Frame& frame);
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

    const uint32_t depthStride = alignUp(DisplayWidth, SCE_GXM_TILE_SIZEX);
    const uint32_t depthHeight = alignUp(DisplayHeight, SCE_GXM_TILE_SIZEY);
    void* depthData = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, depthStride * depthHeight * 4,
                               SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_RW, &depthUid);
    int result = sceGxmDepthStencilSurfaceInit(&depthSurface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32,
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
        { nfs3_shader_glide_v, &glideVId, "glide_v" }, { nfs3_shader_glide_f, &glideFId, "glide_f" },
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
    SceGxmVertexAttribute glideAttributes[5];
    glideAttributes[0] = { 0, 0,  SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, uint16_t(resourceIndex(glideVProgram, "aPosition")) };
    glideAttributes[1] = { 0, 12, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, uint16_t(resourceIndex(glideVProgram, "aColor")) };
    glideAttributes[2] = { 0, 16, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, uint16_t(resourceIndex(glideVProgram, "aTexCoord")) };
    glideAttributes[3] = { 0, 24, SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, uint16_t(resourceIndex(glideVProgram, "aCombine")) };
    glideAttributes[4] = { 0, 40, SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, uint16_t(resourceIndex(glideVProgram, "aAtlasInfo")) };
    SceGxmVertexStream glideStream = { sizeof(Vertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    result = sceGxmShaderPatcherCreateVertexProgram(patcher, glideVId, glideAttributes, 5, &glideStream, 1, &glideV);
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
    result = sceGxmShaderPatcherCreateFragmentProgram(patcher, glideFId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                      SCE_GXM_MULTISAMPLE_NONE, &blend, glideVProgram, &glideFBlend);
    if (result < 0)
    {
        logError("glide blend fragment program creation failed: 0x%08x", result);
        return false;
    }
    blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE;
    result = sceGxmShaderPatcherCreateFragmentProgram(patcher, glideFId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                      SCE_GXM_MULTISAMPLE_NONE, &blend, glideVProgram, &glideFAdd);
    if (result < 0)
    {
        logError("glide additive fragment program creation failed: 0x%08x", result);
        return false;
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
    quadVertices = static_cast<QuadVertex*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, sizeof(QuadVertex) * 8, 4,
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

    for (uint32_t i = 0; i < FrameSlots; ++i)
    {
        Frame& frame = frames[i];
        frame.vertices = static_cast<Vertex*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, sizeof(Vertex) * MaxVertices, 4,
                                                       SCE_GXM_MEMORY_ATTRIB_READ, &frameVertexUid[i]));
        frame.framebuffer = static_cast<uint8_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, FramebufferBytes,
                                                           SCE_GXM_TEXTURE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_READ, &frameFramebufferUid[i]));
        frame.palette = static_cast<uint32_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, PaletteBytes,
                                                        SCE_GXM_PALETTE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_READ, &framePaletteUid[i]));
        frame.drawCalls.reserve(256);
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
        readySlots.wait();
        if (!running)
            break;
        Frame& frame = frames[renderSlot];
        renderFrame(frame);
        renderSlot = (renderSlot + 1) % FrameSlots;
        freeSlots.post();
    }
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

void Device::Impl::renderFrame(Frame& frame)
{
    if (frame.kind == FrameEmpty)
    {
        s_completedFrameIndex.store(frame.frameIndex);
        return;
    }

    sceGxmBeginScene(context, 0, renderTarget, nullptr, nullptr, displaySync[backBuffer], &colorSurface[backBuffer], &depthSurface);
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetViewport(context, 0.5f * DisplayWidth, 0.5f * DisplayWidth, 0.5f * DisplayHeight, -0.5f * DisplayHeight, 0.0f, 1.0f);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, DisplayWidth - 1, DisplayHeight - 1);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);

    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (frame.kind == FrameFramebuffer16 || frame.kind == FrameFramebuffer8)
    {
        SceGxmTexture texture;
        SceGxmTextureFormat format = frame.kind == FrameFramebuffer16 ? SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB : SCE_GXM_TEXTURE_FORMAT_P8_ABGR;
        sceGxmTextureInitLinear(&texture, frame.framebuffer, format, frame.sourceWidth, frame.sourceHeight, 1);
        if (frame.kind == FrameFramebuffer8)
            sceGxmTextureSetPalette(&texture, frame.palette);
        sceGxmTextureSetMinFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetUAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
        drawQuad(0, nullptr, clearV, clearF, black);
        drawQuad(4, &texture, blitV, blitF, nullptr);
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
            drawQuad(0, nullptr, clearV, clearF, black);
            drawQuad(4, nullptr, clearV, clearF, color);
        }
        else
        {
            // No clear: keep drawing over the previously displayed frame.
            SceGxmTexture previous;
            sceGxmTextureInitLinear(&previous, displayData[frontBuffer], SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, DisplayWidth, DisplayHeight, 1);
            sceGxmTextureSetMinFilter(&previous, SCE_GXM_TEXTURE_FILTER_POINT);
            sceGxmTextureSetMagFilter(&previous, SCE_GXM_TEXTURE_FILTER_POINT);
            drawQuad(0, &previous, blitV, blitF, nullptr);
        }

        if (frame.vertexCount)
        {
            const float sw = float(frame.sourceWidth ? frame.sourceWidth : 640);
            const float sh = float(frame.sourceHeight ? frame.sourceHeight : 480);
            const float viewportUniform[4] = {
                2.0f * float(viewport.w) / (sw * DisplayWidth),
                -2.0f * float(viewport.h) / (sh * DisplayHeight),
                2.0f * float(viewport.x) / DisplayWidth - 1.0f,
                1.0f - 2.0f * float(viewport.y) / DisplayHeight };

            sceGxmSetVertexProgram(context, glideV);
            void* uniforms = nullptr;
            sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
            sceGxmSetUniformDataF(uniforms, glideViewportParam, 0, 4, viewportUniform);
            sceGxmSetFragmentTexture(context, glideTextureUnit, &atlasTexture);
            sceGxmSetVertexStream(context, 0, frame.vertices);
            sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, viewport.x, viewport.y,
                                viewport.x + viewport.w - 1, viewport.y + viewport.h - 1);

            uint32_t first = 0;
            for (const DrawCall& call : frame.drawCalls)
            {
                uint32_t count = call.lastVertex - first;
                if (count)
                {
                    sceGxmSetFrontDepthFunc(context, call.zTest ? SCE_GXM_DEPTH_FUNC_LESS_EQUAL : SCE_GXM_DEPTH_FUNC_ALWAYS);
                    sceGxmSetFrontDepthWriteEnable(context, call.zWrite ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
                    sceGxmSetFragmentProgram(context, call.alphaBlend ? glideFBlend : glideFAdd);
                    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, triangleIndices + first, count);
                }
                first = call.lastVertex;
            }
            sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, DisplayWidth - 1, DisplayHeight - 1);
        }
    }

    sceGxmEndScene(context, nullptr, nullptr);
    present(frame);
}

void Device::Impl::present(Frame& frame)
{
    sceGxmPadHeartbeat(&colorSurface[backBuffer], displaySync[backBuffer]);
    DisplayData data;
    data.address = displayData[backBuffer];
    data.vblanks = frame.vblanks;
    data.frameIndex = frame.frameIndex;
    sceGxmDisplayQueueAddEntry(displaySync[frontBuffer], displaySync[backBuffer], &data);
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
    ,   m_atlas(nullptr)
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

    m_atlas = static_cast<uint32_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, AtlasSize * AtlasSize * 4,
                                              SCE_GXM_TEXTURE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_READ, &m_impl->atlasUid));
    sceGxmTextureInitLinear(&m_impl->atlasTexture, m_atlas, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, AtlasSize, AtlasSize, 1);
    sceGxmTextureSetMinFilter(&m_impl->atlasTexture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&m_impl->atlasTexture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&m_impl->atlasTexture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&m_impl->atlasTexture, SCE_GXM_TEXTURE_ADDR_CLAMP);

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
    if (impl.context)
    {
        sceGxmFinish(impl.context);
        sceGxmDisplayQueueFinish();
    }
    if (impl.patcher)
    {
        if (impl.glideV) sceGxmShaderPatcherReleaseVertexProgram(impl.patcher, impl.glideV);
        if (impl.glideFBlend) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, impl.glideFBlend);
        if (impl.glideFAdd) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, impl.glideFAdd);
        if (impl.blitV) sceGxmShaderPatcherReleaseVertexProgram(impl.patcher, impl.blitV);
        if (impl.blitF) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, impl.blitF);
        if (impl.clearV) sceGxmShaderPatcherReleaseVertexProgram(impl.patcher, impl.clearV);
        if (impl.clearF) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, impl.clearF);
        SceGxmShaderPatcherId ids[] = { impl.glideVId, impl.glideFId, impl.blitVId, impl.blitFId, impl.clearVId, impl.clearFId };
        for (SceGxmShaderPatcherId id : ids)
            if (id) sceGxmShaderPatcherUnregisterProgram(impl.patcher, id);
        sceGxmShaderPatcherDestroy(impl.patcher);
    }
    gpuUsseFree(impl.patcherVertexUsseUid, true);
    gpuUsseFree(impl.patcherFragmentUsseUid, false);
    gpuFree(impl.patcherBufferUid);
    for (uint32_t i = 0; i < FrameSlots; ++i)
    {
        gpuFree(impl.frameVertexUid[i]);
        gpuFree(impl.frameFramebufferUid[i]);
        gpuFree(impl.framePaletteUid[i]);
    }
    gpuFree(impl.atlasUid);
    gpuFree(impl.quadVerticesUid);
    gpuFree(impl.quadIndicesUid);
    gpuFree(impl.triangleIndicesUid);
    for (uint32_t i = 0; i < DisplayBuffers; ++i)
    {
        if (impl.displaySync[i]) sceGxmSyncObjectDestroy(impl.displaySync[i]);
        gpuFree(impl.displayUid[i]);
    }
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
    m_atlas = nullptr;
}

Frame& Device::acquireFrame()
{
    Impl& impl = *m_impl;
    if (!impl.frameAcquired)
    {
        impl.freeSlots.wait();
        Frame& frame = impl.frames[impl.acquireSlot];
        frame.kind = FrameEmpty;
        frame.vblanks = 1;
        frame.frameIndex = impl.nextFrameIndex++;
        frame.sourceWidth = 0;
        frame.sourceHeight = 0;
        frame.clear = false;
        frame.clearColor = 0;
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
    impl.readySlots.post();
}

uint32_t Device::completedFrameIndex() const
{
    return s_completedFrameIndex.load();
}

uint32_t Device::recordingFrameIndex() const
{
    Impl& impl = *m_impl;
    return impl.frameAcquired ? impl.frames[impl.acquireSlot].frameIndex : impl.nextFrameIndex;
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
