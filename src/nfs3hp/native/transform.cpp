// Native vertex transform and projection (sub_4bf4c0) and the matrix leaves sub_4e03b0, sub_4e0430, sub_4e0210,
// sub_4e0280 of nfs3.exe.
//
// The emulated x87 works in double precision; the code below performs the same double operations in the same order,
// so the results are bit-identical. Single precision is used only for one operation on two floats rounded to float,
// which equals the double-rounded result (53 >= 2 * 24 + 2). Only NaN payloads can differ (operand order).

#include "guest.h"

namespace nfs3hp { namespace native {

namespace
{

// Projection constants and screen bounds read by sub_4bf4c0.
const x86::reg32 ScaleX = 0x56009c;
const x86::reg32 ScaleY = 0x5600a0;
const x86::reg32 CentreX = 0x5600a4;
const x86::reg32 CentreY = 0x5600a8;
const x86::reg32 Projection = ScaleX;       // ScaleX..CentreY, 16 bytes
const x86::reg32 WLimit = 0x7d34f8;         // outcode 0x10 when 1/z (as int) reaches it
const x86::reg32 XMin = 0x7d34fc;           // outcode 1 below
const x86::reg32 YMax = 0x7d3500;           // outcode 4 above
const x86::reg32 YMin = 0x7d3504;           // outcode 8 below
const x86::reg32 XMax = 0x7d350c;           // outcode 2 above
const x86::reg32 Bounds = WLimit;           // WLimit..XMax, 24 bytes

const x86::reg32 FloatOne = 0x3f800000;
const x86::reg32 ZForOne = 0x37800080;      // z exactly 1.0 is replaced by this before the division

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

// Row vector times 3x3 matrix (rows of three floats), as the guest sums: ((x*m0 + y*m3) + z*m6).
struct Matrix
{
    double m[9];

    void load(x86::reg32 address)
    {
        for (int i = 0; i < 9; ++i)
            m[i] = ldd(address + 4 * i);
    }
    void apply(x86::reg32 vector, double out[3]) const
    {
        const double x = ldd(vector), y = ldd(vector + 4), z = ldd(vector + 8);
        for (int c = 0; c < 3; ++c)
            out[c] = (x * m[c] + y * m[3 + c]) + z * m[6 + c];
    }
};

struct Translation
{
    double t[3];

    void load(x86::reg32 address)
    {
        for (int i = 0; i < 3; ++i)
            t[i] = ldd(address + 4 * i);
    }
};

struct Projector
{
    double scaleX, scaleY, centreX, centreY;
    x86::sreg32 wLimit, xMin, xMax, yMin, yMax;

    void loadProjection()
    {
        scaleX = ldd(ScaleX);
        scaleY = ldd(ScaleY);
        centreX = ldd(CentreX);
        centreY = ldd(CentreY);
    }
    void loadBounds()
    {
        wLimit = x86::sreg32(rc::ld32(WLimit));
        xMin = x86::sreg32(rc::ld32(XMin));
        xMax = x86::sreg32(rc::ld32(XMax));
        yMin = x86::sreg32(rc::ld32(YMin));
        yMax = x86::sreg32(rc::ld32(YMax));
    }
};

// Screen outcode of a projected record from the bit patterns of w, y and x (signed integer compares, negative
// floats always outside).
inline x86::reg32 outcode(const Projector& p, x86::sreg32 w, x86::sreg32 y, x86::sreg32 x)
{
    if (w < 0 || w >= p.wLimit)
        return 0x10;
    x86::reg32 code = 0;
    if (y < 0 || y < p.yMin)
        code |= 8;
    else if (y > p.yMax)
        code |= 4;
    if (x < 0 || x < p.xMin)
        code |= 1;
    else if (x > p.xMax)
        code |= 2;
    return code;
}

}

// Transforms and projects count vertices: eax = count, edx = vec3 array (stride 12), ebx = 3x3 matrix,
// ecx = translation, [esp + 4] = output records (stride 0x20: x, y, z, 1/z, -, outcode byte at 0x14); ret 4.
// The guest overlaps the loop iterations (vertex i is projected after vertex i + 1 is transformed); the native code
// keeps that order and, when the output records overlap an input, rereads the inputs where the guest reads them.
void sub_4bf4c0(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::sreg32 count = x86::sreg32(cpu.eax);
    const x86::reg32 vectors = cpu.edx;
    const x86::reg32 matrixAddress = cpu.ebx;
    const x86::reg32 translationAddress = cpu.ecx;
    const x86::reg32 records = rc::ld32(cpu.esp + 4);
    cpu.esp += 8;
    if (count <= 0)
        return;

    const uint64_t written = uint64_t(count) * 0x20;
    const bool alias = overlaps(records, written, vectors, uint64_t(count) * 12)
                       || overlaps(records, written, matrixAddress, 36)
                       || overlaps(records, written, translationAddress, 12)
                       || overlaps(records, written, Projection, 16)
                       || overlaps(records, written, Bounds, 24);
    Matrix matrix;
    Translation translation;
    Projector projector;
    matrix.load(matrixAddress);
    translation.load(translationAddress);
    projector.loadProjection();
    projector.loadBounds();

    // Transform of vertex i into record i: x, y, z rounded to float, z == 1.0 replaced.
    auto transform = [&](x86::sreg32 i) {
        if (alias && i > 0)
        {
            matrix.load(matrixAddress);
            translation.load(translationAddress);
        }
        const x86::reg32 r = records + 0x20 * x86::reg32(i);
        double v[3];
        matrix.apply(vectors + 12 * x86::reg32(i), v);
        for (int c = 0; c < 3; ++c)
            rc::stf32(r + 4 * c, float(v[c] + translation.t[c]));
        if (rc::ld32(r + 8) == FloatOne)
            rc::st32(r + 8, ZForOne);
    };
    // Projection of record r with w = 1/z: x and y scaled by w and centred, w stored, outcode stored. Leaves the
    // registers as the guest's outcode code does.
    auto project = [&](x86::reg32 r, float w) {
        if (alias)
        {
            projector.scaleX = ldd(ScaleX);
            projector.scaleY = ldd(ScaleY);
        }
        const double wx = projector.scaleX * double(w);
        const double wy = projector.scaleY * double(w);
        const double y = wy * ldd(r + 4);
        rc::stf32(r + 0xc, w);
        const double x = wx * ldd(r);
        const x86::reg32 z = rc::ld32(r + 8);
        if (alias)
        {
            projector.centreX = ldd(CentreX);
            projector.centreY = ldd(CentreY);
        }
        const float sx = float(x + projector.centreX);
        const float sy = float(y + projector.centreY);
        rc::stf32(r, sx);
        rc::st32(r + 8, z);
        rc::stf32(r + 4, sy);
        return z;
    };
    auto code = [&](x86::reg32 r, x86::reg32 z) {
        if (alias)
            projector.loadBounds();
        const x86::reg32 w = rc::ld32(r + 0xc);
        const x86::reg32 y = rc::ld32(r + 4);
        const x86::reg32 x = rc::ld32(r);
        const x86::reg32 result = outcode(projector, x86::sreg32(w), x86::sreg32(y), x86::sreg32(x));
        rc::st8(r + 0x14, x86::reg8(result));
        cpu.ebx = result;
        if (result == 0x10)
        {
            cpu.eax = w;
            cpu.ecx = x86::reg32(projector.wLimit);
            cpu.edx = z;
        }
        else
        {
            cpu.eax = x;
            cpu.ecx = x86::reg32(projector.xMin);
            cpu.edx = x86::reg32(projector.xMax);
        }
    };

    transform(0);
    float w = 1.0f / rc::ldf32(records + 8);
    for (x86::sreg32 i = 1; i < count; ++i)
    {
        const x86::reg32 previous = records + 0x20 * x86::reg32(i - 1);
        transform(i);
        const x86::reg32 z = project(previous, w);
        w = 1.0f / rc::ldf32(previous + 0x28);
        code(previous, z);
    }
    const x86::reg32 last = records + 0x20 * x86::reg32(count - 1);
    code(last, project(last, w));
}

// Rotates count vectors: eax = count, edx = vec3 array, ebx = 3x3 matrix, ecx = output vec3 array.
void sub_4e03b0(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::sreg32 count = x86::sreg32(cpu.eax);
    x86::reg32 in = cpu.edx;
    x86::reg32 out = cpu.ecx;
    const x86::reg32 matrixAddress = cpu.ebx;
    cpu.eax = in;
    cpu.esp += 4;
    if (count <= 0)
        return;
    const bool alias = overlaps(out, uint64_t(count) * 12, matrixAddress, 36);
    Matrix matrix;
    matrix.load(matrixAddress);
    for (x86::sreg32 i = 0; i < count; ++i, in += 12, out += 12)
    {
        if (alias)
            matrix.load(matrixAddress);
        double v[3];
        matrix.apply(in, v);
        for (int c = 0; c < 3; ++c)
            rc::stf32(out + 4 * c, float(v[c]));
    }
    cpu.eax = in;
    cpu.ebx = out - 12;
    cpu.ecx = out;
    cpu.edx = matrixAddress;
}

// Rotates and translates count vectors: eax = count, edx = vec3 array, ebx = 3x3 matrix, ecx = translation,
// [esp + 4] = output vec3 array; ret 4. The rotated vector is stored, then reread and translated.
void sub_4e0430(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::sreg32 count = x86::sreg32(cpu.eax);
    x86::reg32 in = cpu.edx;
    const x86::reg32 matrixAddress = cpu.ebx;
    const x86::reg32 translationAddress = cpu.ecx;
    x86::reg32 out = rc::ld32(cpu.esp + 4);
    cpu.esp += 8;
    cpu.ecx = out;
    if (count <= 0)
        return;
    const uint64_t written = uint64_t(count) * 12;
    const bool alias = overlaps(out, written, matrixAddress, 36) || overlaps(out, written, translationAddress, 12);
    Matrix matrix;
    matrix.load(matrixAddress);
    for (x86::sreg32 i = 0; i < count; ++i, in += 12, out += 12)
    {
        if (alias)
            matrix.load(matrixAddress);
        double v[3];
        matrix.apply(in, v);
        for (int c = 0; c < 3; ++c)
            rc::stf32(out + 4 * c, float(v[c]));
        float translated[3];
        for (int c = 0; c < 3; ++c)
            translated[c] = rc::ldf32(out + 4 * c) + rc::ldf32(translationAddress + 4 * c);
        for (int c = 0; c < 3; ++c)
            rc::stf32(out + 4 * c, translated[c]);
    }
    cpu.eax = out;
    cpu.ebx = out - 12;
    cpu.ecx = out;
    cpu.edx = translationAddress;
}

// Dot products of count vectors with one vector: eax = count, edx = vec3 array, ebx = vec3, ecx = float output
// array.
void sub_4e0210(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::sreg32 count = x86::sreg32(cpu.eax);
    x86::reg32 in = cpu.edx;
    x86::reg32 out = cpu.ecx;
    const x86::reg32 vector = cpu.ebx;
    cpu.esp += 4;
    if (count <= 0)
        return;
    const bool alias = overlaps(out, uint64_t(count) * 4, vector, 12);
    double a[3] = { ldd(vector), ldd(vector + 4), ldd(vector + 8) };
    for (x86::sreg32 i = 0; i < count; ++i, in += 12, out += 4)
    {
        if (alias)
            for (int c = 0; c < 3; ++c)
                a[c] = ldd(vector + 4 * c);
        const double x = ldd(in) * a[0], y = ldd(in + 4) * a[1], z = ldd(in + 8) * a[2];
        rc::stf32(out, float(z + (y + x)));
    }
    cpu.eax = in - 12;
    cpu.ecx = out;
    cpu.edx = vector;
}

// 3x3 matrix product C = A * B: eax = A, edx = B, ebx = C. A or B equal to C is read from a copy taken first (the
// guest copies it to its stack frame, whose address it leaves in eax or edx).
void sub_4e0280(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 a = cpu.eax, b = cpu.edx, c = cpu.ebx;
    const x86::reg32 frameCopy = cpu.esp - 0x3c;
    float copy[9];
    const bool copyA = a == c;
    const bool copyB = b == c;
    if (copyA || copyB)
        memcpy(copy, host(c), sizeof(copy));
    auto elementA = [&](int i) { return copyA ? double(copy[i]) : ldd(a + 4 * i); };
    auto elementB = [&](int i) { return copyB ? double(copy[i]) : ldd(b + 4 * i); };
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
        {
            const double sum = elementA(3 * row + 1) * elementB(3 + column) + elementA(3 * row) * elementB(column);
            rc::stf32(c + 4 * (3 * row + column), float(sum + elementA(3 * row + 2) * elementB(6 + column)));
        }
    if (copyA)
        cpu.eax = frameCopy;
    if (copyB)
        cpu.edx = frameCopy;
    cpu.esp += 4;
}

} }
