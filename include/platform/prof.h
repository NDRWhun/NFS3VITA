#ifndef PLATFORM_PROF_H_
#define PLATFORM_PROF_H_

// Sampling profiler of NFS3_PROFILE builds. The build force-includes this header into instrumented copies of the
// generated code (tools/profile_instrument.py); nothing else in the generated code depends on it.

#include <cpu.h>
#include <lib/memmap.h>
#include <lib/winapp.h>

namespace pf { namespace prof {

// Word of the guest thread block (fs, 4 KB, WinApplication::runThread) holding the guest function the thread runs,
// or the target of the indirect call it is in. The guest only uses fs:[0]; the runtime uses fs+4 and fs+8.
const x86::reg32 SlotOffset = 0xffc;

// Generated code: at function entry and after every call returns.
inline void set(const x86::CPU& cpu, x86::reg32 address)
{
    *reinterpret_cast<volatile x86::reg32*>(win32::g_guestBase + cpu.efs + SlotOffset) = address;
}

// Generated code: indirect call. The slot keeps the target while it is a host shim; guest code overwrites it.
inline void dynamicCall(win32::WinApplication* app, x86::reg32 target, x86::CPU& cpu)
{
    set(cpu, target);
    app->dynamic_call(target, cpu);
}

// Main thread, after the methods are registered: starts the sampler thread. Guest threads are registered by a
// link-time wrapper of WinApplication::runThread (prof.cpp).
void start(const win32::WinApplication& app);
// Main thread, before the application is destroyed: stops the sampler and writes the last report.
void stop();

}}

#endif
