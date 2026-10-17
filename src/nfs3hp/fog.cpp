#include "fog.h"
#include <voodoo2a.h>
#include <lib/winapp.h>
#include <platform/log.h>

// _THRASH_setstate@8 (voodoo2a RVA 0x2790) with state 0xe and a non-zero density calls grFogMode(GR_FOG_WITH_TABLE),
// builds a 64-entry fog table on its stack (65 guest pow and exp, RVA 0x1670) and passes it to grFogTable. It
// writes no guest globals and returns 0. nfs3.exe sends it up to 3 times per rendered view (sub_47a880). The host's
// grFogMode, grFogTable and grFogColorValue do nothing, so the hook returns what the original returns without
// building the table. Density 0 (fog off) and all other states go to the original. nfs3.exe calls it through the
// pointer [0x9ef96c], so every call goes through dynamic_call; voodoo2a's own reset (RVA 0x1f80) calls it directly
// with density 0 only.

namespace nfs3hp { namespace fog {

namespace
{

const x86::reg32 StateFogDensity = 0xe;

win32::MethodPtr s_originalSetState = nullptr;

void thrashSetState(win32::WinApplication* app, x86::CPU& cpu)
{
    // stdcall: return slot at [esp], then state and value; the original pops them (ret 8).
    const x86::reg32 state = app->getMemory<x86::reg32>(cpu.esp + 4);
    const x86::reg32 value = app->getMemory<x86::reg32>(cpu.esp + 8);
    if (state == StateFogDensity && value != 0)
    {
        cpu.eax = 0;
        cpu.esp += 4 + 8;
        return;
    }
    s_originalSetState(app, cpu);
}

}

void install(win32::WinApplication& app)
{
    const x86::reg32 setState = (*voodoo2a::s_registry)["_THRASH_setstate@8"].first;
    s_originalSetState = app.replaceMethod(setState, &thrashSetState);
    pf::logInfo("fog: THRASH fog tables skipped (host fog not implemented)");
}

}}
