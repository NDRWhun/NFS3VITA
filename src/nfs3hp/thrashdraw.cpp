#include "thrashdraw.h"
#include "voodoo2aexport.h"
#include <nfs3hp_layout.h>
#include <lib/gliderenderer.h>
#include <lib/winapp.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <cstring>

// The THRASH draw exports (voodoo2a) write their triangles straight into the frame's GXM vertex buffer, with the
// vertices the original's GrVertex and grDrawTriangle path produces:
// - record: +0 x, +4 y, +8 z, +0xc w (1/z), +0x10 B, G, R, A bytes, +0x18 u, +0x1c v;
// - x, y through double (fld/fstp); oow = w copied;
// - ooz = z * [DepthScale]; sow, tow = u, v * (w * [TextureScale]), products in double, each rounded to float once;
// - r, g, b, a = [ColorTable + 4 * byte] = the bytes themselves (THRASH fills the table with float(i), RVA 0x1d50).
// Triangles: drawtri (v0, v1, v2); drawquad and each drawquadmesh quad (v0, v1, v2), (v2, v3, v0); drawtrifan
// (v0, vi, vi+1) for i = 1..count.
// The original runs when THRASH routes triangles elsewhere (multipass and antialiasing wrappers, RVA 0x26e0), the
// colour table is not filled, there is no open Glide window or the count exceeds a frame. Called by guest
// code only, under the execution lock. eax, ecx, edx and the x87 stack are left unchanged.

namespace nfs3hp { namespace thrashdraw {

namespace
{

// voodoo2a guest addresses (image base + RVA).
const x86::reg32 Voodoo2a             = nfs3hp::layout::voodoo2aBase;
const x86::reg32 GrDrawTriangle       = Voodoo2a + 0x114a8;     // Glide grDrawTriangle, from GetProcAddress
const x86::reg32 TriangleFunction     = Voodoo2a + 0x11510;     // called per triangle by drawtri and drawtrifan
const x86::reg32 QuadTriangleFunction = Voodoo2a + 0x1150c;     // called per triangle by drawquad and drawquadmesh
const x86::reg32 ColorTable           = Voodoo2a + 0x11010;     // 256 floats
const x86::reg32 DepthScale           = Voodoo2a + 0x1015c;     // float
const x86::reg32 TextureScale         = Voodoo2a + 0x10158;     // float
const x86::reg32 RecordSize           = 0x20;

win32::MethodPtr s_originalTri = nullptr;
win32::MethodPtr s_originalQuad = nullptr;
win32::MethodPtr s_originalFan = nullptr;
win32::MethodPtr s_originalQuadMesh = nullptr;

inline x86::reg32 guestWord(x86::reg32 address)
{
    x86::reg32 value;
    memcpy(&value, win32::g_guestBase + address, sizeof(value));
    return value;
}

inline float guestFloat(x86::reg32 address)
{
    float value;
    memcpy(&value, win32::g_guestBase + address, sizeof(value));
    return value;
}

inline x86::reg32 bits(float value)
{
    x86::reg32 result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

// The x87 load and store of a float: the round trip through double quiets a signalling NaN, as in the emulated code.
inline x86::reg32 viaDouble(float value)
{
    volatile double wide = value;
    return bits(float(wide));
}

// One GXM vertex as words (pf::gxm::Vertex layout).
struct Words
{
    x86::reg32 w[9];
};

// Values that are constant during one export call.
struct Context
{
    win32::GlideRenderer* renderer;
    float depthScale;
    double textureScale;
    x86::reg32 colorCombine;
    x86::reg32 alphaCombine;
};

// Fills the context when the export can write the vertices directly.
bool directPath(x86::reg32 trianglePointer, Context& context)
{
    context.renderer = win32::GlideRenderer::active();
    if (!context.renderer || guestWord(trianglePointer) != guestWord(GrDrawTriangle) ||
        guestFloat(ColorTable + 4) != 1.0f || guestFloat(ColorTable + 4 * 255) != 255.0f)
        return false;
    context.depthScale = guestFloat(DepthScale);
    context.textureScale = double(guestFloat(TextureScale));
    context.colorCombine = bits(context.renderer->colorCombine());
    context.alphaCombine = bits(context.renderer->alphaCombine());
    return true;
}

inline void convert(const Context& context, x86::reg32 record, Words& out)
{
    const x86::reg8* p = win32::g_guestBase + record;
    x86::reg32 w, bgra;
    float x, y, z, wf, u, v;
    memcpy(&x, p, 4);
    memcpy(&y, p + 4, 4);
    memcpy(&z, p + 8, 4);
    memcpy(&w, p + 0xc, 4);
    memcpy(&wf, p + 0xc, 4);
    memcpy(&bgra, p + 0x10, 4);
    memcpy(&u, p + 0x18, 4);
    memcpy(&v, p + 0x1c, 4);
    const double scaledW = double(wf) * context.textureScale;
    out.w[0] = viaDouble(x);
    out.w[1] = viaDouble(y);
    out.w[2] = bits(z * context.depthScale);
    out.w[3] = (bgra & 0xff00ff00u) | ((bgra >> 16) & 0xffu) | ((bgra & 0xffu) << 16);
    out.w[4] = bits(float(double(u) * scaledW));
    out.w[5] = bits(float(scaledW * double(v)));
    out.w[6] = context.colorCombine;
    out.w[7] = context.alphaCombine;
    out.w[8] = w;
}

inline void store(pf::gxm::Vertex* vertex, const Words& in)
{
    x86::reg32* out = reinterpret_cast<x86::reg32*>(vertex);
    for (int i = 0; i < 9; ++i)
        out[i] = in.w[i];
}

inline void storeTriangle(pf::gxm::Vertex* out, const Words& a, const Words& b, const Words& c)
{
    store(out, a);
    store(out + 1, b);
    store(out + 2, c);
}

// _THRASH_drawtri@12 (RVA 0x3570): three record pointers.
void drawTri(win32::WinApplication* app, x86::CPU& cpu)
{
    Context context;
    if (!directPath(TriangleFunction, context))
    {
        s_originalTri(app, cpu);
        return;
    }
    Words v0, v1, v2;
    convert(context, guestWord(cpu.esp + 4), v0);
    convert(context, guestWord(cpu.esp + 8), v1);
    convert(context, guestWord(cpu.esp + 12), v2);
    storeTriangle(context.renderer->appendTriangles(1), v0, v1, v2);
    cpu.esp += 4 + 12;
}

// _THRASH_drawquad@16 (RVA 0x3100): four record pointers.
void drawQuad(win32::WinApplication* app, x86::CPU& cpu)
{
    Context context;
    if (!directPath(QuadTriangleFunction, context))
    {
        s_originalQuad(app, cpu);
        return;
    }
    Words v0, v1, v2, v3;
    convert(context, guestWord(cpu.esp + 4), v0);
    convert(context, guestWord(cpu.esp + 8), v1);
    convert(context, guestWord(cpu.esp + 12), v2);
    convert(context, guestWord(cpu.esp + 16), v3);
    pf::gxm::Vertex* out = context.renderer->appendTriangles(2);
    storeTriangle(out, v0, v1, v2);
    storeTriangle(out + 3, v2, v3, v0);
    cpu.esp += 4 + 16;
}

// _THRASH_drawtrifan@8 (RVA 0x3e00): triangle count, then count + 2 consecutive records.
void drawFan(win32::WinApplication* app, x86::CPU& cpu)
{
    Context context;
    if (!directPath(TriangleFunction, context))
    {
        s_originalFan(app, cpu);
        return;
    }
    const int32_t count = int32_t(guestWord(cpu.esp + 4));
    if (count > int32_t(pf::gxm::MaxVertices / 3))
    {
        s_originalFan(app, cpu);
        return;
    }
    x86::reg32 record = guestWord(cpu.esp + 8);
    cpu.esp += 4 + 8;
    if (count <= 0)
        return;
    Words first, previous, next;
    convert(context, record, first);
    convert(context, record + RecordSize, previous);
    pf::gxm::Vertex* out = context.renderer->appendTriangles(x86::reg32(count));
    for (int32_t i = 0; i < count; ++i, out += 3)
    {
        record += RecordSize;
        convert(context, record + RecordSize, next);
        storeTriangle(out, first, previous, next);
        previous = next;
    }
}

// _THRASH_drawquadmesh@12 (RVA 0x3320): quad count, record array, then four record indices per quad.
void drawQuadMesh(win32::WinApplication* app, x86::CPU& cpu)
{
    Context context;
    if (!directPath(QuadTriangleFunction, context))
    {
        s_originalQuadMesh(app, cpu);
        return;
    }
    const int32_t count = int32_t(guestWord(cpu.esp + 4));
    if (count > int32_t(pf::gxm::MaxVertices / 6))
    {
        s_originalQuadMesh(app, cpu);
        return;
    }
    const x86::reg32 records = guestWord(cpu.esp + 8);
    x86::reg32 indices = guestWord(cpu.esp + 12);
    cpu.esp += 4 + 12;
    if (count <= 0)
        return;
    pf::gxm::Vertex* out = context.renderer->appendTriangles(2 * x86::reg32(count));
    for (int32_t i = 0; i < count; ++i, indices += 16, out += 6)
    {
        Words v0, v1, v2, v3;
        convert(context, records + RecordSize * guestWord(indices), v0);
        convert(context, records + RecordSize * guestWord(indices + 4), v1);
        convert(context, records + RecordSize * guestWord(indices + 8), v2);
        convert(context, records + RecordSize * guestWord(indices + 12), v3);
        storeTriangle(out, v0, v1, v2);
        storeTriangle(out + 3, v2, v3, v0);
    }
}

win32::MethodPtr replace(win32::WinApplication& app, const char* name, win32::MethodPtr method)
{
    return app.replaceMethod(voodoo2aExport(name), method);
}

}

void install(win32::WinApplication& app)
{
    s_originalTri = replace(app, "_THRASH_drawtri@12", &drawTri);
    s_originalQuad = replace(app, "_THRASH_drawquad@16", &drawQuad);
    s_originalFan = replace(app, "_THRASH_drawtrifan@8", &drawFan);
    s_originalQuadMesh = replace(app, "_THRASH_drawquadmesh@12", &drawQuadMesh);
    pf::logInfo("thrashdraw: THRASH triangle exports write GXM vertices directly");
}

}}
