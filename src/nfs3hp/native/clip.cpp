// Native quad submit (sub_4c12e0) and polygon clipper (sub_4c2cd0) of nfs3.exe. Both draw through the THRASH
// exports the game resolved into its pointer table, called as guest functions with cdecl arguments on the guest
// stack. Registers and memory (outputs, the clipper's global scratch lists, the frames the THRASH calls see) are
// left exactly as the translated bodies leave them. x87 arithmetic is reproduced as in transform.cpp.

#include "guest.h"
#include <initializer_list>

namespace nfs3hp { namespace native {

namespace
{

const x86::reg32 DepthScale = 0x560094;
const x86::reg32 DepthOffset = 0x560098;
const x86::reg32 ClipYMax = 0x7d34dc;           // outcode 4 above
const x86::reg32 ClipYMin = 0x7d34e0;           // outcode 8 below
const x86::reg32 ClipXMax = 0x7d34ec;           // outcode 2 above
const x86::reg32 ClipNear = 0x7d34f0;           // w of the near plane (outcode 0x10)
const x86::reg32 ClipXMin = 0x7d34f4;           // outcode 1 below

const x86::reg32 DrawQuad = 0x9ef94c;           // THRASH pointer table slots
const x86::reg32 DrawTri = 0x9ef974;
const x86::reg32 DrawTriFan = 0x9ef960;

const x86::reg32 InputList = 0x5600d4;          // pointers to the three clip vertices built from the input
const x86::reg32 InputVertices = 0x7d3570;      // 3 clip vertices
const x86::reg32 CodeScratch = 0x7d0db0;        // outcode lists of the passes, one after the other
const x86::reg32 ListScratch = 0x7d0e78;        // vertex pointer lists of the passes
const x86::reg32 Pool = 0x7d1198;               // clip vertices made by the passes
const x86::reg32 FanVertices = 0x7cdbb0;        // output records for drawtrifan

// Clip vertex: nine floats.
enum Field { X, Y, W, UW, VW, C24, C16, C8, C0, Fields };
const x86::reg32 ClipVertexSize = 4 * Fields;

inline double ldd(x86::reg32 address)
{
    return double(rc::ldf32(address));
}

inline x86::reg32 bits(float value)
{
    x86::reg32 result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

// fld dword + fstp dword: the round trip through double, which quiets a signaling NaN (or gives the default NaN).
inline float viaDouble(float value)
{
    volatile double wide = value;
    return float(wide);
}

// The pattern the game tests for an invalid coordinate: the x87 default NaN with either sign.
inline bool defaultNan(x86::reg32 word)
{
    return (word | 0x80000000u) == 0xffc00000u;
}

// True when w (1/z) is outside [0, 1], tested on the bit pattern: the depth term is then 0, else 1 - w.
inline bool depthClamped(x86::reg32 w)
{
    return x86::sreg32(w) < 0 || x86::sreg32(w) > 0x3f800000;
}

inline float depthTerm(x86::reg32 w)
{
    float value;
    memcpy(&value, &w, sizeof(value));
    return depthClamped(w) ? 0.0f : 1.0f - value;
}

// z = offset + scale * term, each step rounded to float as the guest stores it.
inline float depth(float term)
{
    const float scaled = rc::ldf32(DepthScale) * term;
    return rc::ldf32(DepthOffset) + scaled;
}

// Byte 0 of a colour channel: fistp with the guest's rounding mode.
inline x86::reg32 channel(const x86::CPU& cpu, x86::reg32 address)
{
    return x86::reg32(x86::fistInteger<x86::sreg32>(x86::roundIntegral(ldd(address), cpu.fpu.control.rc))) & 0xff;
}

// Pushes cdecl arguments below esp and calls the THRASH export in a pointer slot.
void callThrash(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 esp, x86::reg32 slot,
                std::initializer_list<x86::reg32> args)
{
    cpu.esp = esp - 4 * x86::reg32(args.size());
    x86::reg32 at = cpu.esp;
    for (x86::reg32 arg : args)
    {
        rc::st32(at, arg);
        at += 4;
    }
    callGuest(app, cpu, rc::ld32(slot));
}

// ---- clipper ------------------------------------------------------------------------------------------------------
// State of the Sutherland-Hodgman passes: the current list (vertex pointers and outcodes) and where the next pass
// writes its list, all in the guest's scratch areas.
struct ClipState
{
    x86::reg32 listIn, codesIn, listOut, codesOut, count, made;
};

// A - (A - B) * t for one field, rounded to float.
inline float interpolate(x86::reg32 a, x86::reg32 b, int field, double t)
{
    const double from = ldd(a + 4 * field);
    return float(from - (from - ldd(b + 4 * field)) * t);
}

// Outcode bits of a new vertex against the screen edges (float compares; NaN counts as below).
inline x86::reg8 yCode(float y)
{
    if (!(y >= rc::ldf32(ClipYMin)))
        return 8;
    return y > rc::ldf32(ClipYMax) ? 4 : 0;
}

inline x86::reg8 xCode(float x)
{
    if (!(x >= rc::ldf32(ClipXMin)))
        return 1;
    return x > rc::ldf32(ClipXMax) ? 2 : 0;
}

// One pass against the plane of outcode bit: vertices with the bit are outside. intersect(a, b, v, codeA) writes the
// crossing of edge a->b into the clip vertex v and returns its outcode. False when fewer than 3 vertices remain.
template<class Intersect>
bool clipPass(ClipState& s, x86::reg8 bit, Intersect intersect)
{
    x86::reg32 out = 0;
    x86::reg32 previous = s.count - 1;
    for (x86::reg32 current = 0; current < s.count; previous = current++)
    {
        const x86::reg8 codeA = rc::ld8(s.codesIn + previous);
        if (!(codeA & bit))
        {
            rc::st32(s.listOut + 4 * out, rc::ld32(s.listIn + 4 * previous));
            rc::st8(s.codesOut + out, codeA);
            ++out;
        }
        if ((codeA ^ rc::ld8(s.codesIn + current)) & bit)
        {
            const x86::reg32 v = Pool + ClipVertexSize * s.made;
            const x86::reg8 code = intersect(rc::ld32(s.listIn + 4 * previous), rc::ld32(s.listIn + 4 * current), v,
                                             codeA);
            rc::st8(s.codesOut + out, code);
            rc::st32(s.listOut + 4 * out, v);
            ++s.made;
            ++out;
        }
    }
    if (out < 3)
        return false;
    s.codesIn = s.codesOut;
    s.listIn = s.listOut;
    s.listOut += 4 * out;
    s.codesOut += out;
    s.count = out;
    return true;
}

x86::reg8 orCodes(const ClipState& s)
{
    x86::reg8 result = 0;
    for (x86::reg32 i = 0; i < s.count; ++i)
        result |= rc::ld8(s.codesIn + i);
    return result;
}

// Near plane w = N. t is computed from 1 / (wA - wB) rounded to float, then rounded to float itself; when it falls
// outside [0, 1] the colours use wA / (wA - wB) instead.
x86::reg8 intersectNear(x86::reg32 a, x86::reg32 b, x86::reg32 v, x86::reg8)
{
    const double wA = ldd(a + 4 * W);
    const float inverse = float(1.0 / (wA - ldd(b + 4 * W)));
    float t = float((wA - ldd(ClipNear)) * double(inverse));
    for (int f : { X, Y, UW, VW })
        rc::stf32(v + 4 * f, interpolate(a, b, f, t));
    if (x86::sreg32(bits(t)) > 0x3f800000 || 0.0 > double(t))
        t = float(wA * double(inverse));
    for (int f : { C24, C16, C8, C0 })
        rc::stf32(v + 4 * f, interpolate(a, b, f, t));
    rc::st32(v + 4 * W, rc::ld32(ClipNear));
    return 0x10 | yCode(rc::ldf32(v + 4 * Y)) | xCode(rc::ldf32(v + 4 * X));
}

// Horizontal edge y = edge: t = (yA - edge) / (yA - yB) in double.
template<x86::reg8 Bit, x86::reg32 Edge>
x86::reg8 intersectY(x86::reg32 a, x86::reg32 b, x86::reg32 v, x86::reg8 codeA)
{
    const double yA = ldd(a + 4 * Y);
    const double t = (yA - ldd(Edge)) / (yA - ldd(b + 4 * Y));
    for (int f : { X, W, UW, VW, C24, C16, C8, C0 })
        rc::stf32(v + 4 * f, interpolate(a, b, f, t));
    rc::stf32(v + 4 * Y, viaDouble(rc::ldf32(Edge)));
    return (codeA & 0xf0) | Bit | xCode(rc::ldf32(v + 4 * X));
}

// Vertical edge x = edge: t = (xA - edge) / (xA - xB) in double.
template<x86::reg8 Bit, x86::reg32 Edge>
x86::reg8 intersectX(x86::reg32 a, x86::reg32 b, x86::reg32 v, x86::reg8 codeA)
{
    const double xA = ldd(a + 4 * X);
    const double t = (xA - ldd(Edge)) / (xA - ldd(b + 4 * X));
    for (int f : { Y, W, UW, VW, C24, C16, C8, C0 })
        rc::stf32(v + 4 * f, interpolate(a, b, f, t));
    rc::stf32(v + 4 * X, viaDouble(rc::ldf32(Edge)));
    return (codeA & 0xfc) | Bit;
}

// Clip vertex k of the input from a projected record: x, y, w, u*w, v*w and the colour bytes as floats.
void buildInputVertex(x86::reg32 record, x86::reg32 v)
{
    const float w = rc::ldf32(record + 0xc);
    rc::stf32(v + 4 * X, viaDouble(rc::ldf32(record)));
    rc::stf32(v + 4 * Y, viaDouble(rc::ldf32(record + 4)));
    rc::stf32(v + 4 * W, viaDouble(w));
    rc::stf32(v + 4 * UW, rc::ldf32(record + 0x18) * w);
    rc::stf32(v + 4 * VW, rc::ldf32(record + 0x1c) * w);
    const x86::reg32 colour = rc::ld32(record + 0x10);
    rc::stf32(v + 4 * C24, float(colour >> 24));
    rc::stf32(v + 4 * C16, float((colour >> 16) & 0xff));
    rc::stf32(v + 4 * C8, float((colour >> 8) & 0xff));
    rc::stf32(v + 4 * C0, float(colour & 0xff));
}

// Projected record (0x20 bytes) from a clip vertex; true when none of x, y, z, w is the invalid pattern.
bool buildFanVertex(const x86::CPU& cpu, x86::reg32 v, x86::reg32 record)
{
    const float w = rc::ldf32(v + 4 * W);
    const float inverse = 1.0f / w;
    rc::stf32(record + 0xc, viaDouble(w));
    rc::st32(record, rc::ld32(v + 4 * X));
    rc::st32(record + 4, rc::ld32(v + 4 * Y));
    rc::stf32(record + 0x18, rc::ldf32(v + 4 * UW) * inverse);
    rc::st32(record + 0x10, channel(cpu, v + 4 * C24) << 24 | channel(cpu, v + 4 * C16) << 16
                            | channel(cpu, v + 4 * C8) << 8 | channel(cpu, v + 4 * C0));
    rc::stf32(record + 8, depth(depthTerm(bits(w))));
    rc::st32(record + 0x14, 0);
    rc::stf32(record + 0x1c, rc::ldf32(v + 4 * VW) * inverse);
    for (x86::reg32 offset = 0; offset < 0x10; offset += 4)
        if (defaultNan(rc::ld32(record + offset)))
            return false;
    return true;
}

}

// Clips a triangle against the near plane and the screen edges, then draws it as a fan: eax = pointer to three
// projected-record pointers, edx = pointer to their three outcodes, bl = their OR. Returns eax = edx = 0 when drawn
// (or when the first vertex is invalid), -1 when clipped away.
void sub_4c2cd0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 entryEsp = cpu.esp;
    const x86::reg32 frame = entryEsp - 0x92;           // ebp
    const x86::reg32 locals = entryEsp - 0xd8;          // esp after the prologue
    const x86::reg32 ecx = cpu.ecx, esi = cpu.esi, edi = cpu.edi, ebp = cpu.ebp;
    auto finish = [&](x86::reg32 result, x86::reg32 ebx) {
        cpu.eax = result;
        cpu.edx = result;
        cpu.ebx = ebx;
        cpu.ecx = ecx;
        cpu.esi = esi;
        cpu.edi = edi;
        cpu.ebp = ebp;
        cpu.esp = entryEsp + 4;
    };

    for (x86::reg32 k = 0; k < 3; ++k)
        buildInputVertex(rc::ld32(cpu.eax + 4 * k), InputVertices + ClipVertexSize * k);
    ClipState s = { InputList, cpu.edx, ListScratch, CodeScratch, 3, 0 };
    x86::reg8 outside = x86::reg8(cpu.ebx);

    if (outside & 0x10)
    {
        if (!clipPass(s, 0x10, intersectNear))
            return finish(0xffffffffu, s.listIn);
        outside = orCodes(s);
    }
    if (outside & 0x0c)
    {
        if ((outside & 4) && !clipPass(s, 4, intersectY<4, ClipYMax>))
            return finish(0xffffffffu, s.listIn);
        if ((outside & 8) && !clipPass(s, 8, intersectY<8, ClipYMin>))
            return finish(0xffffffffu, s.listIn);
        outside = orCodes(s);
    }
    if ((outside & 1) && !clipPass(s, 1, intersectX<1, ClipXMin>))
        return finish(0xffffffffu, s.listIn);
    if ((outside & 2) && !clipPass(s, 2, intersectX<2, ClipXMax>))
        return finish(0xffffffffu, s.listIn);

    const x86::reg32 first = rc::ld32(s.listIn);
    for (x86::reg32 offset = 0; offset < 0xc; offset += 4)
        if (defaultNan(rc::ld32(first + offset)))
            return finish(0, s.listIn);
    x86::reg32 drawn = 0;
    x86::reg32 vertex = esi;
    for (x86::reg32 i = 0; i < s.count; ++i)
    {
        vertex = rc::ld32(s.listIn + 4 * i);
        drawn += buildFanVertex(cpu, vertex, FanVertices + 0x20 * drawn);
    }

    cpu.eax = drawn - 2;
    cpu.ebx = s.listIn + 4 * s.count;
    cpu.ecx = FanVertices + 0x20 * drawn;
    cpu.edx = 0;
    cpu.esi = vertex;
    cpu.edi = drawn;
    cpu.ebp = frame;
    callThrash(app, cpu, locals, DrawTriFan, { drawn - 2, FanVertices });
    if (cpu.terminate)
        return;
    finish(0, cpu.ebx);
}

// Draws a quad of projected records (eax, edx, ebx, ecx = vertices 0-3): rejected when all outcodes share a bit,
// drawn as a quad when all are zero, else as triangles (0, 1, 2) and (0, 2, 3), each drawn, rejected or clipped.
// Before drawing it replaces each vertex's z by its depth. Registers end as the translated body leaves them.
void sub_4c12e0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 entryEsp = cpu.esp;
    const x86::reg32 frame = entryEsp - 12;             // ebp
    const x86::reg32 locals = frame - 0x3c;             // esp after the prologue
    const x86::reg32 esi = cpu.esi, edi = cpu.edi, ebp = cpu.ebp;
    const x86::reg32 v0 = cpu.eax, v1 = cpu.edx, v2 = cpu.ebx, v3 = cpu.ecx;
    const x86::reg8 c0 = rc::ld8(v0 + 0x14), c1 = rc::ld8(v1 + 0x14), c2 = rc::ld8(v2 + 0x14),
                    c3 = rc::ld8(v3 + 0x14);
    rc::st8(frame - 4, c0);
    rc::st8(frame - 3, c1);
    rc::st8(frame - 2, c2);
    rc::st8(frame - 1, c3);
    x86::reg32 eax, ebx = v2, ecx = v3, edx = v1;
    // The guest leaves pushed drawtri arguments on the stack until its epilogue: calls start from the esp left.
    x86::reg32 esp = locals;
    auto finish = [&]() {
        cpu.eax = eax;
        cpu.ebx = ebx;
        cpu.ecx = ecx;
        cpu.edx = edx;
        cpu.esi = esi;
        cpu.edi = edi;
        cpu.ebp = ebp;
        cpu.esp = entryEsp + 4;
    };
    // After a guest call the registers are what it left (esi, edi, ebp are the frame's again).
    auto reload = [&]() {
        esp = cpu.esp;
        eax = cpu.eax;
        ebx = cpu.ebx;
        ecx = cpu.ecx;
        edx = cpu.edx;
    };
    // Depth of one vertex; the guest zeroes the given register when it clamps the term to 0. Returns the term.
    auto setDepth = [&](x86::reg32 v, x86::reg32& zeroed) {
        const x86::reg32 w = rc::ld32(v + 0xc);
        const float term = depthTerm(w);
        if (depthClamped(w))
            zeroed = 0;
        rc::stf32(v + 8, depth(term));
        return bits(term);
    };
    // The validity test before drawtri: eax holds the last word tested (with the sign bit set).
    auto valid = [&](x86::reg32 v) {
        for (x86::reg32 offset : { 0x0u, 0x4u, 0x8u, 0xcu, 0x18u, 0x1cu })
        {
            eax = rc::ld32(v + offset) | 0x80000000u;
            if (eax == 0xffc00000u)
                return false;
        }
        return true;
    };
    auto setRegisters = [&]() {
        cpu.eax = eax;
        cpu.ebx = ebx;
        cpu.ecx = ecx;
        cpu.edx = edx;
        cpu.esi = v2;
        cpu.edi = v0;
        cpu.ebp = frame;
    };
    // Clips triangle (a, b, c) with outcodes already in the frame.
    auto clip = [&](x86::reg32 a, x86::reg32 b, x86::reg32 c) {
        rc::st32(locals, a);
        rc::st32(locals + 4, b);
        rc::st32(locals + 8, c);
        eax = locals;
        edx = frame - 4;
        setRegisters();
        cpu.esp = esp - 4;
        sub_4c2cd0(app, cpu);
    };

    const x86::reg8 all012 = c2 & (c0 & c1);
    eax = (v0 & 0xffff0000u) | x86::reg32(all012) << 8 | (c3 & all012);
    if (c3 & all012)
        return finish();
    const x86::reg8 any012 = c0 | c1 | c2;
    eax = (eax & 0xffffff00u) | any012;
    ebx = (v2 & 0xffffff00u) | (c3 | any012);
    if ((c3 | any012) == 0)
    {
        setDepth(v0, ebx);
        setDepth(v1, ecx);
        setDepth(v2, eax);
        eax = setDepth(v3, ebx);
        ebx = v3;
        setRegisters();
        callThrash(app, cpu, esp, DrawQuad, { v0, v1, v2, v3 });
        if (cpu.terminate)
            return;
        reload();
        return finish();
    }

    // Triangle (0, 1, 2).
    if (all012 == 0)
    {
        if (any012)
        {
            ebx = any012;
            clip(v0, v1, v2);
            if (cpu.terminate)
                return;
            reload();
        }
        else
        {
            setDepth(v0, ebx);
            setDepth(v1, ecx);
            eax = setDepth(v2, eax);
            if (valid(v0) && valid(v1) && valid(v2))
            {
                setRegisters();
                callThrash(app, cpu, esp, DrawTri, { v0, v1, v2 });
                if (cpu.terminate)
                    return;
                reload();
            }
        }
    }

    // Triangle (0, 2, 3).
    edx = (edx & 0xffffff00u) | c3;
    const x86::reg8 all023 = c0 & c2 & c3;
    eax = (eax & 0xffff0000u) | x86::reg32(c2) << 8 | all023;
    if (all023)
        return finish();
    const x86::reg8 any023 = c0 | c2 | c3;
    eax = (eax & 0xffff0000u) | x86::reg32(any023) << 8 | (c0 | c2);
    if (any023)
    {
        rc::st8(frame - 3, c2);
        rc::st8(frame - 2, c3);
        eax = (eax & 0xffffff00u) | c3;
        ebx = any023;
        clip(v0, v2, v3);
        if (cpu.terminate)
            return;
        reload();
        return finish();
    }
    setDepth(v0, ecx);
    setDepth(v2, eax);
    setDepth(v3, ecx);
    edx = v3;
    if (!valid(v0) || !valid(v2) || !valid(v3))
        return finish();
    setRegisters();
    callThrash(app, cpu, esp, DrawTri, { v0, v2, v3 });
    if (cpu.terminate)
        return;
    reload();
    finish();
}

} }
