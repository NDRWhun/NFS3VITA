// Native memcpy/memmove (sub_4ea4f0 and the MMX copy core sub_5033e5 / sub_5033e4) and memset (sub_4e070c,
// sub_4e0716, sub_4e0721) of nfs3.exe.

#include "guest.h"

namespace nfs3hp { namespace native {

namespace
{

const x86::reg32 UseMmxCopy = 0x5643b4;     // non-zero: sub_4ea4f0 copies with the MMX core sub_5033e5
const x86::reg32 UseFildCopy = 0x5643ac;    // else non-zero: the fild/fistp core sub_503281, zero: dword core

// The copy cores as written (head to 8-byte alignment, 128/32/8-byte blocks, dword/word/byte tail, all counts
// signed as in the guest), in the guest's load and store order, so any overlap gives the guest's bytes. Returns the
// last values loaded into mm0..mm6 in mm and the registers that were loaded in the bit mask.
unsigned copyForwardAsWritten(x86::CPU& cpu, uint64_t* mm)
{
    x86::reg32 d = cpu.eax;
    x86::reg32 s = cpu.edx;
    x86::reg32 n = cpu.ebx;
    unsigned loaded = 0;
    if (d & 7)
    {
        if ((d & 1) && x86::sreg32(n) >= 1)
        {
            rc::st8(d, rc::ld8(s));
            d += 1; s += 1; n -= 1;
        }
        if ((d & 2) && x86::sreg32(n) >= 2)
        {
            rc::st16(d, rc::ld16(s));
            d += 2; s += 2; n -= 2;
        }
        if ((d & 4) && x86::sreg32(n) >= 4)
        {
            rc::st32(d, rc::ld32(s));
            d += 4; s += 4; n -= 4;
        }
    }
    n -= 0x80;
    if (x86::sreg32(n) >= 0)
    {
        loaded = 0x7f;
        do
        {
            // mm0, mm4, mm5, mm6 hold the first qword of each 32-byte quarter, mm1-mm3 the other three.
            mm[0] = rc::ld64(s);
            mm[4] = rc::ld64(s + 0x20);
            mm[5] = rc::ld64(s + 0x40);
            mm[6] = rc::ld64(s + 0x60);
            for (unsigned quarter = 0; quarter < 4; ++quarter)
            {
                const x86::reg32 offset = quarter * 0x20;
                mm[1] = rc::ld64(s + offset + 0x08);
                mm[2] = rc::ld64(s + offset + 0x10);
                mm[3] = rc::ld64(s + offset + 0x18);
                rc::st64(d + offset, mm[quarter == 0 ? 0 : 3 + quarter]);
                rc::st64(d + offset + 0x08, mm[1]);
                rc::st64(d + offset + 0x10, mm[2]);
                rc::st64(d + offset + 0x18, mm[3]);
            }
            s += 0x80; d += 0x80; n -= 0x80;
        } while (x86::sreg32(n) >= 0);
    }
    n += 0x60;
    if (x86::sreg32(n) >= 0)
    {
        loaded |= 0xf;
        do
        {
            mm[0] = rc::ld64(s);
            mm[1] = rc::ld64(s + 0x08);
            mm[2] = rc::ld64(s + 0x10);
            mm[3] = rc::ld64(s + 0x18);
            rc::st64(d, mm[0]);
            rc::st64(d + 0x08, mm[1]);
            rc::st64(d + 0x10, mm[2]);
            rc::st64(d + 0x18, mm[3]);
            s += 0x20; d += 0x20; n -= 0x20;
        } while (x86::sreg32(n) >= 0);
    }
    n += 0x18;
    if (x86::sreg32(n) >= 0)
    {
        loaded |= 1;
        do
        {
            mm[0] = rc::ld64(s);
            rc::st64(d, mm[0]);
            s += 8; d += 8; n -= 8;
        } while (x86::sreg32(n) >= 0);
    }
    n += 8;
    if (n != 0)
    {
        if (n >= 4)
        {
            rc::st32(d, rc::ld32(s));
            d += 4; s += 4; n -= 4;
        }
        if (n >= 2)
        {
            rc::st16(d, rc::ld16(s));
            d += 2; s += 2; n -= 2;
        }
        if (n >= 1)
        {
            rc::st8(d, rc::ld8(s));
            d += 1; s += 1; n -= 1;
        }
    }
    cpu.eax = d;
    cpu.edx = s;
    cpu.ebx = n;
    return loaded;
}

// Last qwords the MMX core loads into mm0..mm6 for a count below 2^31 (returns the mask of registers loaded). Its
// blocks start after the head that aligns the destination to 8 bytes.
unsigned lastLoads(x86::reg32 d, x86::reg32 s, x86::reg32 n, uint64_t* mm)
{
    for (x86::reg32 size = 1; size <= 4; size *= 2)
        if ((d & size) && n >= size)
        {
            d += size; s += size; n -= size;
        }
    unsigned loaded = 0;
    if (n >= 0x80)
    {
        const x86::reg32 b = s + (n / 0x80 - 1) * 0x80;
        const x86::reg32 offsets[7] = { 0, 0x68, 0x70, 0x78, 0x20, 0x40, 0x60 };
        for (int i = 0; i < 7; ++i)
            mm[i] = rc::ld64(b + offsets[i]);
        loaded = 0x7f;
    }
    s += n / 0x80 * 0x80;
    n %= 0x80;
    if (n >= 0x20)
    {
        const x86::reg32 b = s + (n / 0x20 - 1) * 0x20;
        for (int i = 0; i < 4; ++i)
            mm[i] = rc::ld64(b + 8 * i);
        loaded |= 0xf;
    }
    s += n / 0x20 * 0x20;
    n %= 0x20;
    if (n >= 8)
    {
        mm[0] = rc::ld64(s + (n / 8 - 1) * 8);
        loaded |= 1;
    }
    return loaded;
}

// sub_5033e5 body: eax = destination, edx = source, ebx = count; ecx is preserved and mm0..mm6 hold the last
// qwords loaded. Unless the destination starts inside the source, every load sees the source as it was, and the
// copy equals memmove.
void copyMmx(x86::CPU& cpu)
{
    uint64_t mm[7];
    const x86::reg32 dst = cpu.eax, src = cpu.edx, n = cpu.ebx;
    unsigned loaded;
    if (x86::sreg32(n) >= 0 && !(dst > src && dst < x86::reg32(src + n)))
    {
        loaded = lastLoads(dst, src, n, mm);
        memmove(host(dst), host(src), n);
        cpu.eax = dst + n;
        cpu.edx = src + n;
        cpu.ebx = 0;
    }
    else
        loaded = copyForwardAsWritten(cpu, mm);
    x86::regmmx* const registers[7] = { &cpu.mmx.mm0, &cpu.mmx.mm1, &cpu.mmx.mm2, &cpu.mmx.mm3,
                                        &cpu.mmx.mm4, &cpu.mmx.mm5, &cpu.mmx.mm6 };
    for (unsigned i = 0; i < 7; ++i)
        if (loaded & (1u << i))
            *registers[i] = x86::from_reg64(mm[i]);
    cpu.esp += 4;
}

// Fills bytes bytes (a multiple of 8) at address with the 4-byte pattern, pattern byte 0 at address.
void fillPattern(x86::reg32 address, x86::reg32 bytes, x86::reg32 pattern)
{
    if (pattern == (pattern & 0xff) * 0x01010101u)
    {
        memset(host(address), int(pattern & 0xff), bytes);
        return;
    }
    const uint64_t pattern64 = uint64_t(pattern) * 0x100000001ull;
    x86::reg8* p = host(address);
    for (x86::reg8* end = p + bytes; p != end; p += 8)
        memcpy(p, &pattern64, 8);
}

// sub_4e0721 body: eax = destination, edx = count (signed), ebx = 32-bit pattern. Stores the pattern bytes where the
// guest does (byte 0 for the single bytes, bytes 0-1 for the words) and leaves eax/edx as the guest does.
void fill(x86::CPU& cpu)
{
    x86::reg32 d = cpu.eax;
    x86::reg32 n = cpu.edx;
    const x86::reg32 pattern = cpu.ebx;
    if (d & 7)
    {
        if ((d & 1) && x86::sreg32(n) >= 1)
        {
            rc::st8(d, x86::reg8(pattern));
            d += 1; n -= 1;
        }
        if ((d & 2) && x86::sreg32(n) >= 2)
        {
            rc::st16(d, x86::reg16(pattern));
            d += 2; n -= 2;
        }
        if ((d & 4) && x86::sreg32(n) >= 4)
        {
            rc::st32(d, pattern);
            d += 4; n -= 4;
        }
    }
    if (x86::sreg32(n) >= 0)
    {
        const x86::reg32 bulk = n & ~7u;
        fillPattern(d, bulk, pattern);
        d += bulk;
        n &= 7;
        if (n & 4)
        {
            rc::st32(d, pattern);
            d += 4;
        }
        if (n & 2)
        {
            rc::st16(d, x86::reg16(pattern));
            d += 2;
        }
        if (n & 1)
            rc::st8(d, x86::reg8(pattern));
    }
    cpu.eax = d;
    cpu.edx = n;
}

}

// memmove: eax = source, edx = destination, ebx = count. ecx is preserved.
void sub_4ea4f0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 src = cpu.eax;
    const x86::reg32 dst = cpu.edx;
    const x86::reg32 n = cpu.ebx;
    if (dst > src && dst < x86::reg32(src + n))
    {
        // Backward byte loop; eax and edx end at the first byte of each block.
        memmove(host(dst), host(src), n);
        cpu.edx = dst;
        cpu.ebx = 0xffffffffu;
        cpu.esp += 4;
        return;
    }
    const bool mmx = rc::ld32(UseMmxCopy) != 0;
    if (!mmx && rc::ld32(UseFildCopy) != 0)
    {
        const x86::reg32 ecx = cpu.ecx;
        cpu.esp -= 4;
        cpu.eax = dst;
        cpu.edx = src;
        cpu.ecx = dst;
        callGuest(app, cpu, 0x503281);
        if (cpu.terminate)
            return;
        cpu.ecx = ecx;
        cpu.esp += 8;
        return;
    }
    if (x86::sreg32(n) >= 0)
    {
        // Both cores copy forward, which equals memmove unless the destination starts inside the source.
        memmove(host(dst), host(src), n);
        cpu.eax = dst + n;
        cpu.edx = src + n;
        cpu.ebx = 0;
    }
    else
    {
        // Negative count: the cores copy a short tail.
        uint64_t mm[7];
        cpu.eax = dst;
        cpu.edx = src;
        copyForwardAsWritten(cpu, mm);
    }
    if (mmx)
        cpu.mmx.init();
    cpu.esp += 4;
}

// MMX copy core: eax = destination, edx = source, ebx = count.
void sub_5033e5(win32::WinApplication*, x86::CPU& cpu)
{
    copyMmx(cpu);
}

// Same with eax and edx swapped on entry.
void sub_5033e4(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 eax = cpu.eax;
    cpu.eax = cpu.edx;
    cpu.edx = eax;
    copyMmx(cpu);
}

// memset with zero: eax = destination, edx = count. ebx is preserved.
void sub_4e070c(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 ebx = cpu.ebx;
    cpu.ebx = 0;
    fill(cpu);
    cpu.ebx = ebx;
    cpu.esp += 4;
}

// memset with the byte bl: copies bl to bh, then ORs bx into the high half of ebx (left in ebx).
void sub_4e0716(win32::WinApplication*, x86::CPU& cpu)
{
    x86::reg32 ebx = (cpu.ebx & 0xffff00ffu) | ((cpu.ebx & 0xff) << 8);
    cpu.ebx = ebx | (ebx << 16);
    fill(cpu);
    cpu.esp += 4;
}

// memset with the 32-bit pattern in ebx.
void sub_4e0721(win32::WinApplication*, x86::CPU& cpu)
{
    fill(cpu);
    cpu.esp += 4;
}

} }
