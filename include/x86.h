#ifndef X86_H_
#define X86_H_

#include    <cstdint>
#include    <cstdlib>

namespace x86
{

// Logs the failed expression, then traps (or continues with NFS3_ASSERT_SOFT).
void assertFail(const char* expression, const char* file, int line);

}

#define NFS2_ASSERT(x)                                              \
    do { if (!(x)) ::x86::assertFail(#x, __FILE__, __LINE__); } while (0)
#define NFS2_USE(x)             \
    (void)x


namespace x86
{

typedef uint8_t reg8;
typedef uint16_t reg16;
typedef uint32_t reg32;
typedef uint64_t reg64;

typedef int8_t sreg8;
typedef int16_t sreg16;
typedef int32_t sreg32;
typedef int64_t sreg64;

struct CPU;
struct FPU;

}

#endif /* !X86_H_ */
