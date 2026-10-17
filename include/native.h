#ifndef NATIVE_H_
#define NATIVE_H_

// Native replacements of guest functions (tools/recompiler/generate.py NATIVE_OVERRIDES). The generated entry of
// an overridden function calls NFS3_NATIVE(address, native::sub_x, sub_x_guest); sub_x_guest is the translated body.

#include <lib/winapp.h>

namespace rc
{

// Runs the native function and checks it against the translated body (NFS3_NATIVE_VERIFY builds).
void verifyNative(x86::reg32 address, win32::MethodPtr native, win32::MethodPtr guest,
                  win32::WinApplication* app, x86::CPU& cpu);

}

#ifdef NFS3_NATIVE_VERIFY
#define NFS3_NATIVE(address, native, guest) ::rc::verifyNative(address, native, guest, app, cpu)
#else
#define NFS3_NATIVE(address, native, guest) native(app, cpu)
#endif

#endif /* !NATIVE_H_ */
