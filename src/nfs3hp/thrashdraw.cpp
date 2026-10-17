#include "thrashdraw.h"
#include <voodoo2a.h>
#include <lib/gliderenderer.h>
#include <lib/winapp.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <cstring>

// The THRASH draw exports (voodoo2a) convert each game vertex record into a Glide GrVertex on their stack and call
// grDrawTriangle through a pointer once per triangle; the host then turns the GrVertex into a GXM vertex. The
// replacements produce the same GXM vertices directly:
// - record: +0 x, +4 y, +8 z, +0xc w (1/z), +0x10 B, G, R, A bytes, +0x18 u, +0x1c v;
// - x, y through double (fld/fstp); oow = w copied;
// - ooz = z * [DepthScale]; sow, tow = u, v * (w * [TextureScale]), the products in double like the x87
//   code, each result rounded to float once (under the same FPSCR as the emulated x87);
// - r, g, b, a = [ColorTable + 4 * byte], a table THRASH fills with float(i) (sub_a83f50): the bytes themselves.
// Triangles: drawtri (v0, v1, v2); drawquad and each drawquadmesh quad (v0, v1, v2), (v2, v3, v0); drawtrifan
// (v0, vi, vi+1) for i = 1..count.
// A replacement runs the original when THRASH routes triangles elsewhere (its multipass and antialiasing wrappers,
// sub_a848e0), while the colour table is not filled, or without an open Glide window. Only guest code calls the
// exports, through the pointer table at 0x9ef9xx (dynamic_call), under the execution lock. eax, ecx and edx (stdcall
// scratch registers) are left unchanged and the x87 stack is untouched; the originals leave it balanced too.

namespace nfs3hp { namespace thrashdraw {

namespace
{

// voodoo2a guest addresses.
const x86::reg32 GrDrawTriangle       = 0xa936a8;   // Glide grDrawTriangle, from GetProcAddress
const x86::reg32 TriangleFunction     = 0xa93710;   // called per triangle by drawtri and drawtrifan
const x86::reg32 QuadTriangleFunction = 0xa9370c;   // called per triangle by drawquad and drawquadmesh
const x86::reg32 ColorTable           = 0xa93210;   // 256 floats
const x86::reg32 DepthScale           = 0xa9235c;   // float
const x86::reg32 TextureScale         = 0xa92358;   // float
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

// _THRASH_drawtri@12 (sub_a85770): three record pointers.
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

// _THRASH_drawquad@16 (sub_a85300): four record pointers.
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

// _THRASH_drawtrifan@8 (sub_a86000): triangle count, then count + 2 consecutive records.
void drawFan(win32::WinApplication* app, x86::CPU& cpu)
{
    Context context;
    if (!directPath(TriangleFunction, context))
    {
        s_originalFan(app, cpu);
        return;
    }
    const int32_t count = int32_t(guestWord(cpu.esp + 4));
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

// _THRASH_drawquadmesh@12 (sub_a85520): quad count, record array, then four record indices per quad.
void drawQuadMesh(win32::WinApplication* app, x86::CPU& cpu)
{
    Context context;
    if (!directPath(QuadTriangleFunction, context))
    {
        s_originalQuadMesh(app, cpu);
        return;
    }
    const int32_t count = int32_t(guestWord(cpu.esp + 4));
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
    return app.replaceMethod((*voodoo2a::s_registry)[name].first, method);
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
