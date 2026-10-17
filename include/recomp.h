#ifndef RECOMP_H_
#define RECOMP_H_

// Support code for the generated game code (tools/recompiler): guest memory loads and stores.

#include <x86.h>
#include <lib/memmap.h>
#include <cstring>

namespace rc
{

template< typename T >
inline T load(x86::reg32 address)
{
    T value;
    memcpy(&value, win32::g_guestBase + address, sizeof(T));
    return value;
}

template< typename T >
inline void store(x86::reg32 address, T value)
{
    memcpy(win32::g_guestBase + address, &value, sizeof(T));
}

inline x86::reg8 ld8(x86::reg32 address)    { return load<x86::reg8>(address); }
inline x86::reg16 ld16(x86::reg32 address)  { return load<x86::reg16>(address); }
inline x86::reg32 ld32(x86::reg32 address)  { return load<x86::reg32>(address); }
inline x86::reg64 ld64(x86::reg32 address)  { return load<x86::reg64>(address); }
inline float ldf32(x86::reg32 address)      { return load<float>(address); }
inline double ldf64(x86::reg32 address)     { return load<double>(address); }

inline void st8(x86::reg32 address, x86::reg8 value)    { store(address, value); }
inline void st16(x86::reg32 address, x86::reg16 value)  { store(address, value); }
inline void st32(x86::reg32 address, x86::reg32 value)  { store(address, value); }
inline void st64(x86::reg32 address, x86::reg64 value)  { store(address, value); }
inline void stf32(x86::reg32 address, float value)      { store(address, value); }
inline void stf64(x86::reg32 address, double value)     { store(address, value); }

}

#endif /* !RECOMP_H_ */
