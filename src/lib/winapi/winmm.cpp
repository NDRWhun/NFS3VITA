#include <winapi/winmm.h>
#include <lib/timer.h>
#include <lib/gamepad.h>
#include <x86.h>

namespace win32 { namespace winmm
{

UINT joyGetNumDevs(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return Gamepad::getCount();
}

// System sounds are not played.
BOOL PlaySoundA(WinApplication* app, x86::CPU& cpu,
                x86::reg32 pszSound, HANDLE hmod, DWORD fdwSound)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(pszSound);
    NFS2_USE(hmod);
    NFS2_USE(fdwSound);
    return 1;
}

MMRESULT timeBeginPeriod(WinApplication* app, x86::CPU& cpu,
                         UINT uPeriod)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(uPeriod);
    return 0;
}

MMRESULT timeEndPeriod(WinApplication* app, x86::CPU& cpu,
                       UINT uPeriod)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_USE(uPeriod);
    return 0;
}

MMRESULT timeGetDevCaps(WinApplication* app, x86::CPU& cpu,
                        LPTIMECAPS ptc, UINT cbtc)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    NFS2_ASSERT(cbtc == sizeof(*ptc));
    ptc->wPeriodMin = 1;
    ptc->wPeriodMax = 1000;
    return 0;
}

// Same clock as GetTickCount.
DWORD timeGetTime(WinApplication* app, x86::CPU& cpu)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    return timeGetTickCount();
}

MMRESULT timeKillEvent(WinApplication* app, x86::CPU& cpu,
                       UINT uTimerID)
{
    NFS2_USE(app);
    NFS2_USE(cpu);
    // TIMERR_NOCANDO (TIMERR_BASE + 1) for an id that is not pending.
    return win32::Timer::kill(uTimerID) ? 0 : 97;
}

MMRESULT timeSetEvent(WinApplication* app, x86::CPU& cpu,
                      UINT uDelay, UINT uResolution, LPTIMECALLBACK lpTimeProc,
                      x86::reg32 dwUser, UINT fuEvent)
{
    NFS2_ASSERT(fuEvent == 0);
    NFS2_USE(uResolution);
    if (cpu.terminate)
        return 0;
    return win32::Timer::set(app, uDelay, lpTimeProc, dwUser);
}

}}
