#include "mastertimer.h"
#include <lib/event.h>
#include <lib/timer.h>
#include <lib/winapp.h>
#include <platform/log.h>

// sub_48ed30, nfs3.exe's only timeSetEvent callback, natively: it advances its own schedule, re-arms itself as a
// one-shot timer while the timer runs, then wakes the master tick thread and two service threads. It runs on the
// timer thread without the execution lock and accesses the guest words (aligned dwords) atomically. The setup
// (sub_48eff0) primes it with a direct guest call, whose timeSetEvent then arms this version.
// While the tick event is still set (the tick thread has not run yet), the fire is retried 1 ms later without
// advancing the schedule, so no tick is lost; the schedule then catches up at 1 kHz.

namespace nfs3hp { namespace mastertimer {

namespace
{

const x86::reg32 Callback       = 0x48ed30;
const x86::reg32 Period         = 0x79c170;    // ms per tick, 16.16 fixed point
const x86::reg32 Schedule       = 0x79c178;    // due time of this tick in GetTickCount ms
const x86::reg32 TickEvent      = 0x79c17c;    // auto-reset event of the tick thread sub_48eb90
const x86::reg32 TimerId        = 0x79c194;    // id of the armed timer, for timeKillEvent
const x86::reg32 Fraction       = 0x79c198;    // schedule fraction, 16 bits
const x86::reg32 Running        = 0x79c19c;    // timer rate in Hz, 0 when stopped
const x86::reg32 TicksPaused    = 0x55e2c4;    // non-zero: no tick event
const x86::reg32 Resyncs        = 0x55e2d0;    // times the schedule was more than 1 s late
const x86::reg32 ServiceEvent   = 0x79c168;    // service thread sub_48ec40
const x86::reg32 StreamEvent    = 0x5652ac;    // service thread sub_4f27a0 (sub_4f2a60)

x86::reg32 load(win32::WinApplication* app, x86::reg32 address)
{
    return __atomic_load_n(&app->getMemory<x86::reg32>(address), __ATOMIC_RELAXED);
}

void store(win32::WinApplication* app, x86::reg32 address, x86::reg32 value)
{
    __atomic_store_n(&app->getMemory<x86::reg32>(address), value, __ATOMIC_RELAXED);
}

// timeSetEvent(delay, 1, sub_48ed30, 0, TIME_ONESHOT). It only fails during shutdown, so the guest's stop on failure
// is left out. The stop (sub_48ef50) reads the timer id, clears Running and kills that id; a timer armed here after
// it read the id would stay pending, so it is killed when Running reads 0 after the arm.
void arm(win32::WinApplication* app, x86::reg32 delayMs)
{
    const x86::reg32 id = win32::Timer::set(app, delayMs, Callback, 0);
    store(app, TimerId, id);
    if (load(app, Running) == 0)
        win32::Timer::kill(id);
}

// The tick event, or 0 when ticks are paused or the tick thread has none.
x86::reg32 tickEvent(win32::WinApplication* app)
{
    const x86::reg32 handle = load(app, TickEvent);
    return handle && load(app, TicksPaused) == 0 ? handle : 0;
}

void setEvent(x86::reg32 handle)
{
    if (handle)
        win32::Event::setHandle(handle);
}

void tick(win32::WinApplication* app)
{
    const bool running = load(app, Running) != 0;
    const x86::reg32 pending = tickEvent(app);
    if (running && pending && win32::Event::isHandleSet(pending))
    {
        arm(app, 1);
        return;
    }

    const x86::reg32 sum = load(app, Period) + load(app, Fraction);
    const x86::reg32 schedule = load(app, Schedule) + x86::reg32(x86::sreg32(sum) >> 16);
    store(app, Fraction, sum & 0xffff);
    store(app, Schedule, schedule);
    if (running)
    {
        x86::sreg32 delay = x86::sreg32(schedule - win32::timeGetTickCount() - 1);
        if (delay < 1)
        {
            if (delay < -1000)
            {
                store(app, Schedule, win32::timeGetTickCount() + 1);
                store(app, Resyncs, load(app, Resyncs) + 1);
            }
            delay = 1;
        }
        arm(app, x86::reg32(delay));
    }
    setEvent(tickEvent(app));
    setEvent(load(app, ServiceEvent));
    setEvent(load(app, StreamEvent));
}

}

void install(win32::WinApplication& app)
{
    NFS2_USE(app);
    win32::Timer::setNativeCallback(Callback, &tick);
    pf::logInfo("master timer: native tick callback");
}

}}
