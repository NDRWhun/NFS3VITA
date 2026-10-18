#ifndef NATIVE_H_
#define NATIVE_H_

// Native replacements of guest functions (tools/recompiler/generate.py NATIVE_OVERRIDES). The generated entry of
// an overridden function calls NFS3_NATIVE(address, native::sub_x, sub_x_guest), which runs the native function;
// address and the translated body sub_x_guest are unused.

#include <lib/winapp.h>

#define NFS3_NATIVE(address, native, guest) native(app, cpu)

#endif /* !NATIVE_H_ */
