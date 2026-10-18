#include <mmx.h>
#include <platform/log.h>
#include <cstdint>
#include <cstring>

// Checks the NEON MMX intrinsics against scalar SSE2 semantics and logs each mismatch.
namespace x86
{

namespace
{

struct Lanes
{
    uint8_t b[16];

    uint16_t u16(int i) const { uint16_t v; memcpy(&v, b + i * 2, 2); return v; }
    uint32_t u32(int i) const { uint32_t v; memcpy(&v, b + i * 4, 4); return v; }
    uint64_t u64(int i) const { uint64_t v; memcpy(&v, b + i * 8, 8); return v; }
    void set16(int i, uint16_t v) { memcpy(b + i * 2, &v, 2); }
    void set32(int i, uint32_t v) { memcpy(b + i * 4, &v, 4); }
    void set64(int i, uint64_t v) { memcpy(b + i * 8, &v, 8); }
};

Lanes fromNeon(__m128i v)
{
    Lanes l;
    vst1q_u8(l.b, v);
    return l;
}

__m128i toNeon(const Lanes& l)
{
    return vld1q_u8(l.b);
}

int  s_failures = 0;

void check(const char* name, __m128i actual, const Lanes& expected)
{
    Lanes got = fromNeon(actual);
    if (memcmp(got.b, expected.b, 16) != 0)
    {
        s_failures++;
        pf::logError("mmx self-test %s: got %016llx%016llx expected %016llx%016llx", name,
                     (unsigned long long)got.u64(1), (unsigned long long)got.u64(0),
                     (unsigned long long)expected.u64(1), (unsigned long long)expected.u64(0));
    }
}

int16_t sat16(int32_t v) { return int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
int8_t  sat8(int32_t v)  { return int8_t(v > 127 ? 127 : (v < -128 ? -128 : v)); }
uint8_t satu8(int32_t v) { return uint8_t(v > 255 ? 255 : (v < 0 ? 0 : v)); }

}

void mmxSelfTest()
{
    Lanes a, b, e;
    for (int i = 0; i < 16; ++i)
    {
        a.b[i] = uint8_t(0x81 + i * 0x1d);
        b.b[i] = uint8_t(0x3c + i * 0x4b);
    }
    __m128i va = toNeon(a), vb = toNeon(b);

    // Scalar moves.
    memset(e.b, 0, 16); e.set32(0, 0x89abcdef);
    check("cvtsi32_si128", _mm_cvtsi32_si128(int32_t(0x89abcdef)), e);
    memset(e.b, 0, 16); e.set64(0, 0x0123456789abcdefull);
    check("cvtsi64_si128", _mm_cvtsi64_si128(int64_t(0x0123456789abcdefull)), e);
    if (uint32_t(_mm_cvtsi128_si32(va)) != a.u32(0)) { s_failures++; pf::logError("mmx self-test cvtsi128_si32"); }
    if (uint64_t(_mm_cvtsi128_si64(va)) != a.u64(0)) { s_failures++; pf::logError("mmx self-test cvtsi128_si64"); }
    uint32_t stored = 0;
    _mm_storeu_si32(&stored, va);
    if (stored != a.u32(0)) { s_failures++; pf::logError("mmx self-test storeu_si32"); }
    memset(e.b, 0, 16); e.set32(0, b.u32(1));
    check("loadu_si32", _mm_loadu_si32(b.b + 4), e);

    // Logic and adds.
    for (int i = 0; i < 16; ++i) e.b[i] = a.b[i] & b.b[i];
    check("and", _mm_and_si128(va, vb), e);
    for (int i = 0; i < 16; ++i) e.b[i] = a.b[i] | b.b[i];
    check("or", _mm_or_si128(va, vb), e);
    for (int i = 0; i < 16; ++i) e.b[i] = a.b[i] ^ b.b[i];
    check("xor", _mm_xor_si128(va, vb), e);
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t(a.u16(i) + b.u16(i)));
    check("add_epi16", _mm_add_epi16(va, vb), e);
    for (int i = 0; i < 4; ++i) e.set32(i, a.u32(i) + b.u32(i));
    check("add_epi32", _mm_add_epi32(va, vb), e);
    for (int i = 0; i < 16; ++i) e.b[i] = uint8_t(sat8(int8_t(a.b[i]) + int8_t(b.b[i])));
    check("adds_epi8", _mm_adds_epi8(va, vb), e);
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t(sat16(int16_t(a.u16(i)) + int16_t(b.u16(i)))));
    check("adds_epi16", _mm_adds_epi16(va, vb), e);

    // Multiplies.
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t((int32_t(int16_t(a.u16(i))) * int16_t(b.u16(i))) >> 16));
    check("mulhi_epi16", _mm_mulhi_epi16(va, vb), e);
    for (int i = 0; i < 4; ++i)
        e.set32(i, uint32_t(int32_t(int16_t(a.u16(2 * i))) * int16_t(b.u16(2 * i)))
                 + uint32_t(int32_t(int16_t(a.u16(2 * i + 1))) * int16_t(b.u16(2 * i + 1))));
    check("madd_epi16", _mm_madd_epi16(va, vb), e);

    // Immediate shifts, in range and past the lane width.
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t(a.u16(i) << 5));
    check("slli_epi16", _mm_slli_epi16(va, 5), e);
    memset(e.b, 0, 16);
    check("slli_epi16 wide", _mm_slli_epi16(va, 16), e);
    for (int i = 0; i < 4; ++i) e.set32(i, a.u32(i) << 7);
    check("slli_epi32", _mm_slli_epi32(va, 7), e);
    for (int i = 0; i < 2; ++i) e.set64(i, a.u64(i) << 32);
    check("slli_epi64", _mm_slli_epi64(va, 32), e);
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t(a.u16(i) >> 3));
    check("srli_epi16", _mm_srli_epi16(va, 3), e);
    memset(e.b, 0, 16);
    check("srli_epi16 wide", _mm_srli_epi16(va, 16), e);
    for (int i = 0; i < 4; ++i) e.set32(i, a.u32(i) >> 16);
    check("srli_epi32", _mm_srli_epi32(va, 16), e);
    for (int i = 0; i < 2; ++i) e.set64(i, a.u64(i) >> 23);
    check("srli_epi64", _mm_srli_epi64(va, 23), e);
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t(int16_t(a.u16(i)) >> 4));
    check("srai_epi16", _mm_srai_epi16(va, 4), e);
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t(int16_t(a.u16(i)) >> 15));
    check("srai_epi16 wide", _mm_srai_epi16(va, 20), e);
    for (int i = 0; i < 4; ++i) e.set32(i, uint32_t(int32_t(a.u32(i)) >> 23));
    check("srai_epi32", _mm_srai_epi32(va, 23), e);

    // Register-count shifts: the count is the whole low 64 bits.
    Lanes count; memset(count.b, 0, 16); count.set64(0, 3);
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t(a.u16(i) << 3));
    check("sll_epi16", _mm_sll_epi16(va, toNeon(count)), e);
    for (int i = 0; i < 4; ++i) e.set32(i, a.u32(i) >> 3);
    check("srl_epi32", _mm_srl_epi32(va, toNeon(count)), e);
    for (int i = 0; i < 2; ++i) e.set64(i, a.u64(i) >> 3);
    check("srl_epi64", _mm_srl_epi64(va, toNeon(count)), e);
    for (int i = 0; i < 8; ++i) e.set16(i, uint16_t(int16_t(a.u16(i)) >> 3));
    check("sra_epi16", _mm_sra_epi16(va, toNeon(count)), e);
    count.set64(0, 0x100000003ull);
    memset(e.b, 0, 16);
    check("sll_epi16 wide", _mm_sll_epi16(va, toNeon(count)), e);

    // Byte shift, unpacks and packs.
    memset(e.b, 0, 16); memcpy(e.b, a.b + 8, 8);
    check("srli_si128", _mm_srli_si128(va, 8), e);
    memset(e.b, 0, 16); memcpy(e.b, a.b + 3, 13);
    check("srli_si128 3", _mm_srli_si128(va, 3), e);
    for (int i = 0; i < 8; ++i) { e.b[2 * i] = a.b[i]; e.b[2 * i + 1] = b.b[i]; }
    check("unpacklo_epi8", _mm_unpacklo_epi8(va, vb), e);
    for (int i = 0; i < 4; ++i) { e.set16(2 * i, a.u16(i)); e.set16(2 * i + 1, b.u16(i)); }
    check("unpacklo_epi16", _mm_unpacklo_epi16(va, vb), e);
    for (int i = 0; i < 2; ++i) { e.set32(2 * i, a.u32(i)); e.set32(2 * i + 1, b.u32(i)); }
    check("unpacklo_epi32", _mm_unpacklo_epi32(va, vb), e);
    for (int i = 0; i < 8; ++i) { e.b[i] = satu8(int16_t(a.u16(i))); e.b[8 + i] = satu8(int16_t(b.u16(i))); }
    check("packus_epi16", _mm_packus_epi16(va, vb), e);
    for (int i = 0; i < 8; ++i) { e.b[i] = uint8_t(sat8(int16_t(a.u16(i)))); e.b[8 + i] = uint8_t(sat8(int16_t(b.u16(i)))); }
    check("packs_epi16", _mm_packs_epi16(va, vb), e);
    for (int i = 0; i < 4; ++i) { e.set16(i, uint16_t(sat16(int32_t(a.u32(i))))); e.set16(4 + i, uint16_t(sat16(int32_t(b.u32(i))))); }
    check("packs_epi32", _mm_packs_epi32(va, vb), e);

    if (s_failures == 0)
        pf::logInfo("mmx self-test passed");
}

}
