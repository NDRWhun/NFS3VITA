#include "videomodes.h"
#include <lib/winapp.h>
#include <nfs3hp_layout.h>
#include <platform/config.h>
#include <platform/log.h>
#include <voodoo2a.h>
#include <winapi/glide2x.h>
#include <cstdio>

// voodoo2a reports its Glide modes from a static table: 16 entries of 40 bytes from index 1 (width, height, bits per
// pixel, flags, colour buffer count, aux buffer count, ...) and the Glide resolution code of each entry, which
// THRASH_setvideomode passes to grSstWinOpen. THRASH_about recomputes the buffer counts from the frame buffer
// memory, and gives 960-pixel-wide modes none (they need two Voodoo2 boards).
//
// The Modern Patch lists the modes from 5:4 to 16:9 whose flags have bit 0 set and bit 1 clear and that have buffers
// (sub_4bee60). At start-up it picks the frontend mode nearest to 640x480 and sets it (sub_517620); then it picks the
// race mode nearest to the one saved in config.dat (640x480 after its first-start reset, sub_448394). Offering a
// single race mode from the first mode set on makes every race choice land on it: the 856x480 entry becomes 960x544
// (opened by the port's grSstWinOpen for GR_RESOLUTION_960x544) and is the only mode listed, unless nfs3vita.cfg
// names another one with resolution=WIDTHxHEIGHT. Bit 1 hides the others from the list only: voodoo2a ignores it,
// and the frontend sets its mode by index.

namespace nfs3hp { namespace videomodes {

namespace
{

const x86::reg32 ModeTable   = layout::voodoo2aBase + 0xfe18;
const x86::reg32 ModeCodes   = layout::voodoo2aBase + 0x100e8;
const x86::reg32 ModeCount   = 16;
const x86::reg32 EntrySize   = 40;
const x86::reg32 Flags       = 0x10;
const x86::reg32 Buffers     = 0x14;
const x86::reg32 AuxBuffers  = 0x18;
const x86::reg32 Hidden      = 2;           // flags bit 1: not listed by the Modern Patch
const x86::reg32 WideEntry   = 11;          // 856x480 in the original table
const x86::reg32 BufferCount = 3;           // what THRASH_about gives the other modes

x86::reg32 s_offered = WideEntry;
bool s_raceOnly = false;                    // after the first mode set (the frontend's)
win32::MethodPtr s_originalAbout = nullptr;
win32::MethodPtr s_originalSetVideoMode = nullptr;

x86::reg32 entry(x86::reg32 index)
{
    return ModeTable + index * EntrySize;
}

// Lists every mode until the frontend mode is set, then only the offered race mode.
void updateFlags(win32::WinApplication* app)
{
    for (x86::reg32 i = 1; i <= ModeCount; ++i)
    {
        auto flags = app->getMemory<x86::reg32>(entry(i) + Flags);
        flags = !s_raceOnly || i == s_offered ? flags & ~Hidden : flags | Hidden;
    }
}

// _THRASH_about@0: no arguments, returns the info block in eax.
void thrashAbout(win32::WinApplication* app, x86::CPU& cpu)
{
    s_originalAbout(app, cpu);
    app->getMemory<x86::reg32>(entry(WideEntry) + Buffers) = BufferCount;
    app->getMemory<x86::reg32>(entry(WideEntry) + AuxBuffers) = BufferCount;
    updateFlags(app);
}

// _THRASH_setvideomode@12 (mode index, buffer counts).
void thrashSetVideoMode(win32::WinApplication* app, x86::CPU& cpu)
{
    s_originalSetVideoMode(app, cpu);
    if (!s_raceOnly)
    {
        s_raceOnly = true;
        updateFlags(app);
    }
}

// Table index of an offered width x height, 0 if there is none.
x86::reg32 findMode(win32::WinApplication& app, x86::reg32 width, x86::reg32 height)
{
    for (x86::reg32 i = 1; i <= ModeCount; ++i)
    {
        const x86::reg32 e = entry(i);
        if (app.getMemory<x86::reg32>(e) == width && app.getMemory<x86::reg32>(e + 4) == height &&
            (app.getMemory<x86::reg32>(e + Flags) & 1))
            return i;
    }
    return 0;
}

}

void install(win32::WinApplication& app)
{
    auto width = app.getMemory<x86::reg32>(entry(WideEntry));
    auto height = app.getMemory<x86::reg32>(entry(WideEntry) + 4);
    auto code = app.getMemory<x86::reg32>(ModeCodes + WideEntry * 4);
    if (width != 856 || height != 480 || code != 0xa)
    {
        pf::logError("video modes: voodoo2a mode %u is %ux%u (code %u), not 856x480; modes left as they are",
                     WideEntry, x86::reg32(width), x86::reg32(height), x86::reg32(code));
        return;
    }
    width = 960;
    height = 544;
    code = win32::glide2x::GR_RESOLUTION_960x544;

    if (const char* resolution = pf::Config::getString("resolution", nullptr))
    {
        unsigned w = 0, h = 0;
        const x86::reg32 index = sscanf(resolution, "%ux%u", &w, &h) == 2 ? findMode(app, w, h) : 0;
        if (index)
            s_offered = index;
        else
            pf::logError("video modes: resolution=%s is not a Voodoo 2 mode, 960x544 used", resolution);
    }
    s_originalAbout = app.replaceMethod((*voodoo2a::s_registry)["_THRASH_about@0"].first, &thrashAbout);
    s_originalSetVideoMode = app.replaceMethod((*voodoo2a::s_registry)["_THRASH_setvideomode@12"].first,
                                               &thrashSetVideoMode);
    pf::logInfo("video modes: race resolution %ux%u", x86::reg32(app.getMemory<x86::reg32>(entry(s_offered))),
                x86::reg32(app.getMemory<x86::reg32>(entry(s_offered) + 4)));
}

}}
