#ifndef MMX_H_
#define MMX_H_

#include <x86.h>
#include <arm_neon.h>
#include <cstring>

// MMX registers as emitted by the recompiler: SSE2 intrinsics on a 128-bit lane
// whose low 64 bits hold the MMX value. Implemented here on NEON Q registers.
typedef uint8x16_t __m128i;

static inline __m128i _mm_setzero_si128()
{
    return vdupq_n_u8(0);
}

static inline __m128i _mm_cvtsi32_si128(int32_t a)
{
    return vreinterpretq_u8_u32(vsetq_lane_u32(uint32_t(a), vdupq_n_u32(0), 0));
}

static inline __m128i _mm_cvtsi64_si128(int64_t a)
{
    return vreinterpretq_u8_u64(vsetq_lane_u64(uint64_t(a), vdupq_n_u64(0), 0));
}

static inline int32_t _mm_cvtsi128_si32(__m128i a)
{
    return vgetq_lane_s32(vreinterpretq_s32_u8(a), 0);
}

static inline int64_t _mm_cvtsi128_si64(__m128i a)
{
    return vgetq_lane_s64(vreinterpretq_s64_u8(a), 0);
}

static inline __m128i _mm_loadu_si32(const void* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return _mm_cvtsi32_si128(int32_t(v));
}

static inline void _mm_storeu_si32(void* p, __m128i a)
{
    uint32_t v = vgetq_lane_u32(vreinterpretq_u32_u8(a), 0);
    memcpy(p, &v, sizeof(v));
}

static inline __m128i _mm_and_si128(__m128i a, __m128i b) { return vandq_u8(a, b); }
static inline __m128i _mm_or_si128(__m128i a, __m128i b)  { return vorrq_u8(a, b); }
static inline __m128i _mm_xor_si128(__m128i a, __m128i b) { return veorq_u8(a, b); }

static inline __m128i _mm_add_epi16(__m128i a, __m128i b)
{
    return vreinterpretq_u8_s16(vaddq_s16(vreinterpretq_s16_u8(a), vreinterpretq_s16_u8(b)));
}

static inline __m128i _mm_add_epi32(__m128i a, __m128i b)
{
    return vreinterpretq_u8_s32(vaddq_s32(vreinterpretq_s32_u8(a), vreinterpretq_s32_u8(b)));
}

static inline __m128i _mm_adds_epi8(__m128i a, __m128i b)
{
    return vreinterpretq_u8_s8(vqaddq_s8(vreinterpretq_s8_u8(a), vreinterpretq_s8_u8(b)));
}

static inline __m128i _mm_adds_epi16(__m128i a, __m128i b)
{
    return vreinterpretq_u8_s16(vqaddq_s16(vreinterpretq_s16_u8(a), vreinterpretq_s16_u8(b)));
}

// Signed 16-bit multiply, high 16 bits of each 32-bit product.
static inline __m128i _mm_mulhi_epi16(__m128i a, __m128i b)
{
    int16x8_t sa = vreinterpretq_s16_u8(a);
    int16x8_t sb = vreinterpretq_s16_u8(b);
    int16x4_t lo = vshrn_n_s32(vmull_s16(vget_low_s16(sa), vget_low_s16(sb)), 16);
    int16x4_t hi = vshrn_n_s32(vmull_s16(vget_high_s16(sa), vget_high_s16(sb)), 16);
    return vreinterpretq_u8_s16(vcombine_s16(lo, hi));
}

// Signed 16-bit multiply, adjacent pairs summed into 32-bit lanes.
static inline __m128i _mm_madd_epi16(__m128i a, __m128i b)
{
    int16x8_t sa = vreinterpretq_s16_u8(a);
    int16x8_t sb = vreinterpretq_s16_u8(b);
    int32x4_t lo = vmull_s16(vget_low_s16(sa), vget_low_s16(sb));
    int32x4_t hi = vmull_s16(vget_high_s16(sa), vget_high_s16(sb));
    int32x2_t r0 = vpadd_s32(vget_low_s32(lo), vget_high_s32(lo));
    int32x2_t r1 = vpadd_s32(vget_low_s32(hi), vget_high_s32(hi));
    return vreinterpretq_u8_s32(vcombine_s32(r0, r1));
}

// Shifts: SSE zeroes the lane (or sign-fills for arithmetic) once the count reaches the lane width.
static inline __m128i _mm_slli_epi16(__m128i a, int n)
{
    if (n > 15) return _mm_setzero_si128();
    return vreinterpretq_u8_u16(vshlq_u16(vreinterpretq_u16_u8(a), vdupq_n_s16(int16_t(n))));
}

static inline __m128i _mm_slli_epi32(__m128i a, int n)
{
    if (n > 31) return _mm_setzero_si128();
    return vreinterpretq_u8_u32(vshlq_u32(vreinterpretq_u32_u8(a), vdupq_n_s32(n)));
}

static inline __m128i _mm_slli_epi64(__m128i a, int n)
{
    if (n > 63) return _mm_setzero_si128();
    return vreinterpretq_u8_u64(vshlq_u64(vreinterpretq_u64_u8(a), vdupq_n_s64(n)));
}

static inline __m128i _mm_srli_epi16(__m128i a, int n)
{
    if (n > 15) return _mm_setzero_si128();
    return vreinterpretq_u8_u16(vshlq_u16(vreinterpretq_u16_u8(a), vdupq_n_s16(int16_t(-n))));
}

static inline __m128i _mm_srli_epi32(__m128i a, int n)
{
    if (n > 31) return _mm_setzero_si128();
    return vreinterpretq_u8_u32(vshlq_u32(vreinterpretq_u32_u8(a), vdupq_n_s32(-n)));
}

static inline __m128i _mm_srli_epi64(__m128i a, int n)
{
    if (n > 63) return _mm_setzero_si128();
    return vreinterpretq_u8_u64(vshlq_u64(vreinterpretq_u64_u8(a), vdupq_n_s64(-n)));
}

static inline __m128i _mm_srai_epi16(__m128i a, int n)
{
    if (n > 15) n = 15;
    return vreinterpretq_u8_s16(vshlq_s16(vreinterpretq_s16_u8(a), vdupq_n_s16(int16_t(-n))));
}

static inline __m128i _mm_srai_epi32(__m128i a, int n)
{
    if (n > 31) n = 31;
    return vreinterpretq_u8_s32(vshlq_s32(vreinterpretq_s32_u8(a), vdupq_n_s32(-n)));
}

// Register-count variants take the count from the low 64 bits of b.
static inline int mmxShiftCount(__m128i b)
{
    uint64_t c = vgetq_lane_u64(vreinterpretq_u64_u8(b), 0);
    return c > 255 ? 255 : int(c);
}

static inline __m128i _mm_sll_epi16(__m128i a, __m128i b) { return _mm_slli_epi16(a, mmxShiftCount(b)); }
static inline __m128i _mm_sll_epi32(__m128i a, __m128i b) { return _mm_slli_epi32(a, mmxShiftCount(b)); }
static inline __m128i _mm_sll_epi64(__m128i a, __m128i b) { return _mm_slli_epi64(a, mmxShiftCount(b)); }
static inline __m128i _mm_srl_epi16(__m128i a, __m128i b) { return _mm_srli_epi16(a, mmxShiftCount(b)); }
static inline __m128i _mm_srl_epi32(__m128i a, __m128i b) { return _mm_srli_epi32(a, mmxShiftCount(b)); }
static inline __m128i _mm_srl_epi64(__m128i a, __m128i b) { return _mm_srli_epi64(a, mmxShiftCount(b)); }
static inline __m128i _mm_sra_epi16(__m128i a, __m128i b) { return _mm_srai_epi16(a, mmxShiftCount(b)); }
static inline __m128i _mm_sra_epi32(__m128i a, __m128i b) { return _mm_srai_epi32(a, mmxShiftCount(b)); }

// Whole-register byte shift right.
static inline __m128i _mm_srli_si128(__m128i a, int bytes)
{
    const uint8x16_t z = vdupq_n_u8(0);
    switch (bytes)
    {
    case 0:  return a;
    case 1:  return vextq_u8(a, z, 1);
    case 2:  return vextq_u8(a, z, 2);
    case 3:  return vextq_u8(a, z, 3);
    case 4:  return vextq_u8(a, z, 4);
    case 5:  return vextq_u8(a, z, 5);
    case 6:  return vextq_u8(a, z, 6);
    case 7:  return vextq_u8(a, z, 7);
    case 8:  return vextq_u8(a, z, 8);
    case 9:  return vextq_u8(a, z, 9);
    case 10: return vextq_u8(a, z, 10);
    case 11: return vextq_u8(a, z, 11);
    case 12: return vextq_u8(a, z, 12);
    case 13: return vextq_u8(a, z, 13);
    case 14: return vextq_u8(a, z, 14);
    case 15: return vextq_u8(a, z, 15);
    default: return z;
    }
}

static inline __m128i _mm_unpacklo_epi8(__m128i a, __m128i b)
{
    return vzipq_u8(a, b).val[0];
}

static inline __m128i _mm_unpacklo_epi16(__m128i a, __m128i b)
{
    return vreinterpretq_u8_u16(vzipq_u16(vreinterpretq_u16_u8(a), vreinterpretq_u16_u8(b)).val[0]);
}

static inline __m128i _mm_unpacklo_epi32(__m128i a, __m128i b)
{
    return vreinterpretq_u8_u32(vzipq_u32(vreinterpretq_u32_u8(a), vreinterpretq_u32_u8(b)).val[0]);
}

static inline __m128i _mm_packus_epi16(__m128i a, __m128i b)
{
    return vcombine_u8(vqmovun_s16(vreinterpretq_s16_u8(a)), vqmovun_s16(vreinterpretq_s16_u8(b)));
}

static inline __m128i _mm_packs_epi16(__m128i a, __m128i b)
{
    return vreinterpretq_u8_s8(vcombine_s8(vqmovn_s16(vreinterpretq_s16_u8(a)), vqmovn_s16(vreinterpretq_s16_u8(b))));
}

static inline __m128i _mm_packs_epi32(__m128i a, __m128i b)
{
    return vreinterpretq_u8_s16(vcombine_s16(vqmovn_s32(vreinterpretq_s32_u8(a)), vqmovn_s32(vreinterpretq_s32_u8(b))));
}

namespace x86
{

struct regmmx
{
    __m128i reg;
    operator x86::reg64() const
    {
        return x86::reg64(_mm_cvtsi128_si64(reg));
    }
    operator __m128i() const
    {
        return reg;
    }
};

static inline regmmx from_reg64(const x86::reg64& value)
{
    regmmx result;
    result.reg = _mm_cvtsi64_si128(int64_t(value));
    return result;
}

struct MMX
{
    regmmx mm0;
    regmmx mm1;
    regmmx mm2;
    regmmx mm3;
    regmmx mm4;
    regmmx mm5;
    regmmx mm6;
    regmmx mm7;
    void init()
    {
        memset(this, 0, sizeof(*this));
    }
};

}

#endif /* !MMX_H_ */
