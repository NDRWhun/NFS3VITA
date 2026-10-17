#ifndef NFS3HP_NATIVE_GUEST_H_
#define NFS3HP_NATIVE_GUEST_H_

// Helpers for the hand-written replacements of guest functions (tools/recompiler NATIVE_OVERRIDES, include/native.h).
// Each replacement keeps the guest ABI: arguments in cpu registers and on the guest stack, the same register,
// memory and esp results as the translated body, and the x87 stack depth unchanged.

#include <lib/winapp.h>
#include <recomp.h>
#include <cstring>

namespace nfs3hp { namespace native {

// Host pointer of a guest address.
inline x86::reg8* host(x86::reg32 address)
{
    return win32::g_guestBase + address;
}

// Calls a guest function by address the way generated code calls through a pointer: the call pushes a return
// address (not stored, as in the generated code) and the callee pops it.
inline void callGuest(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 address)
{
    cpu.ip = address;
    cpu.esp -= 4;
    app->dynamic_call(address, cpu);
}

// True when [a, a + aSize) and [b, b + bSize) share a byte (sizes in bytes, 64-bit so they cannot wrap).
inline bool overlaps(x86::reg32 a, uint64_t aSize, x86::reg32 b, uint64_t bSize)
{
    return a < b + bSize && b < a + aSize;
}

} }

#endif /* !NFS3HP_NATIVE_GUEST_H_ */
