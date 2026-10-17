#ifndef glide2x_H_
#define glide2x_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32
{

class WinApplication;
class Library;

}

namespace win32 { namespace glide2x
{

extern Library* s_glide2xRegistry;

// Resolution code outside the Glide 2.x range that grSstWinOpen accepts for the Vita display size.
static const x86::reg32 GR_RESOLUTION_960x544 = 0x80;

}}

#endif
