#include "loadingscreen.h"
#include "native/guest.h"

// The Modern Patch draws its loading screen into the race-resolution frame (sub_494cb0): it reads the 4:3 box of
// the race view (0x7a2740: x, y, width, height, set by the view layout sub_4be5e0) with sub_4beb20 and places the
// 640x480 slide, the progress bar and the progress text in it, scaled by width / 640 and height / 480. That is
// the only read of 0x7a2740 through sub_4beb20 (the others read the 2D view 0x7cdac0; the original exe never passes
// 0x7a2740), and the HUD reads the box directly. With stretch on, sub_4beb20 returns the whole screen for it
// (0x7cda68/0x7cda6c: screen width and height), so the slide and everything placed on it fill the display.

namespace nfs3hp {

namespace {

const x86::reg32 LoadingBox   = 0x7a2740;
const x86::reg32 ScreenWidth  = 0x7cda68;
const x86::reg32 ScreenHeight = 0x7cda6c;

bool s_stretch = false;

}

namespace loadingscreen {

void setStretch(bool stretch)
{
    s_stretch = stretch;
}

}

namespace native {

// sub_4beb20(eax = rect struct, edx = &x, ebx = &y, ecx = &width, [esp + 4] = &height); ret 4. The guest reads
// and writes in this order, keeps esi and ebp, and returns with edx = &height and eax = height.
void sub_4beb20(win32::WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    const x86::reg32 rect = cpu.eax;
    const bool full = s_stretch && rect == LoadingBox;
    rc::st32(cpu.edx, full ? 0 : rc::ld32(rect));
    rc::st32(cpu.ebx, full ? 0 : rc::ld32(rect + 4));
    rc::st32(cpu.ecx, full ? rc::ld32(ScreenWidth) : rc::ld32(rect + 8));
    const x86::reg32 heightOut = rc::ld32(cpu.esp + 4);
    const x86::reg32 height = full ? rc::ld32(ScreenHeight) : rc::ld32(rect + 12);
    rc::st32(heightOut, height);
    cpu.edx = heightOut;
    cpu.eax = height;
    cpu.esp += 8;
}

}

}
