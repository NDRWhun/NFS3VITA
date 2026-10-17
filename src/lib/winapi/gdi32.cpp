#include <winapi/gdi32.h>
#include <x86.h>
#include <platform/log.h>

namespace win32 { namespace gdi32
{

// The desktop is the 16-bit Vita display.
int GetDeviceCaps(WinApplication* app, x86::CPU& cpu, HDC hdc, int index)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(hdc);
    enum { HORZRES = 8, VERTRES = 10, BITSPIXEL = 12, PLANES = 14 };
    switch (index)
    {
    case HORZRES:
        return 960;
    case VERTRES:
        return 544;
    case BITSPIXEL:
        return 16;
    case PLANES:
        return 1;
    default:
        pf::logError("GetDeviceCaps: index %d not implemented", index);
        return 0;
    }
}

HGDIOBJ GetStockObject(WinApplication* app, x86::CPU& cpu, int i)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(i);
    return 0xffffffff;
}

}}
