#include <fpu.h>
#include <platform/log.h>
#include <cfloat>
#include <cstdint>
#include <cstring>

// Checks the inline x87 rounding against libm in all four rounding modes and logs each mismatch.
namespace x86
{

namespace
{

int s_checked = 0;
int s_failures = 0;
uint64_t s_random = 0x9e3779b97f4a7c15ull;

uint64_t toBits(double value) { uint64_t bits; memcpy(&bits, &value, sizeof(bits)); return bits; }
double fromBits(uint64_t bits) { double value; memcpy(&value, &bits, sizeof(value)); return value; }

uint64_t nextRandom()
{
    s_random ^= s_random << 13;
    s_random ^= s_random >> 7;
    s_random ^= s_random << 17;
    return s_random;
}

void check(double value)
{
    for (reg32 mode = 0; mode < 4; ++mode)
    {
        // volatile keeps the compiler from evaluating either side at build time
        volatile double input = value;
        volatile reg32 inputMode = mode;
        const double expected = roundIntegralLibm(input, inputMode);
        const double actual = roundIntegral(input, inputMode);
        ++s_checked;
        if (toBits(actual) != toBits(expected) && ++s_failures <= 20)
        {
            pf::logError("fpu self-test mode %u: %.17g (%016llx) gave %016llx, libm %016llx", unsigned(mode), value,
                         (unsigned long long)toBits(value), (unsigned long long)toBits(actual),
                         (unsigned long long)toBits(expected));
        }
    }
}

// The value, its neighbours and their negations.
void checkAround(double value)
{
    const double below = nextafter(value, 0.0);
    const double above = nextafter(value, INFINITY);
    check(value); check(-value);
    check(below); check(-below);
    check(above); check(-above);
}

}

void fpuSelfTest()
{
    static const double s_edges[] = {
        0.0, DBL_TRUE_MIN, 0x1p-1060, DBL_MIN, 0x1p-60, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 2.5, 3.5,
        0.49999999999999994, 123.456, 1e6 + 0.5, 1e9 + 0.5,
        0x1p30 + 0.5, 0x1p31 - 1.5, 0x1p31 - 1.0, 0x1p31 - 0.5, 0x1p31, 0x1p31 + 0.5, 0x1p32 + 0.5,
        0x1p51 + 0.5, 0x1p51 + 1.5, 0x1p52 - 1.5, 0x1p52 - 0.5, 0x1p52, 0x1p52 + 1.0, 0x1p53, 0x1p63, 1e300, DBL_MAX,
    };
    for (double value : s_edges)
        checkAround(value);
    check(INFINITY);
    check(-INFINITY);
    check(fromBits(0x7ff8000000000000ull));   // quiet NaN
    check(fromBits(0xfff8000000001234ull));   // negative NaN with payload
    check(fromBits(0x7ff0000000000001ull));   // signalling NaN
    // Quarters and halves around zero.
    for (int i = -2048; i <= 2048; ++i)
    {
        check(i * 0.25);
        check(i + 0.5);
    }
    for (int i = 0; i < 8192; ++i)
    {
        // Any bit pattern.
        check(fromBits(nextRandom()));
        // Magnitudes 2^-64 .. 2^64, spanning both inline limits (2^31 and 2^52).
        const uint64_t r = nextRandom();
        check(fromBits((r & 0x800fffffffffffffull) | ((1023 - 64 + (r >> 57)) << 52)));
        // Integers of up to 2^33 plus eighths.
        const uint64_t s = nextRandom();
        check(double(int64_t(s) >> 30) + double((s >> 3) & 7) * 0.125);
    }
    // Denormals.
    for (int i = 0; i < 1024; ++i)
        check(fromBits(nextRandom() & 0x800fffffffffffffull));

    if (s_failures == 0)
        pf::logInfo("fpu self-test passed (%d roundings)", s_checked);
    else
        pf::logError("fpu self-test: %d of %d roundings differ from libm", s_failures, s_checked);
}

}
