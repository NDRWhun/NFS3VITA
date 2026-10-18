#include "gamespeed.h"
#include <lib/winapp.h>
#include <platform/log.h>
#include <platform/perf.h>

// Race time in nfs3.exe: the 128 Hz master timer thread counts ticks, every 4th tick wakes the input thread, which
// queues one input frame per wake-up, and the race loop runs two 64 Hz simulation steps per queued input frame.
// The counters are read without the execution lock; aligned words, only used for the log.

namespace nfs3hp { namespace gamespeed {

namespace
{

const x86::reg32 SimSteps    = 0x7d3684;    // race simulation steps, 64 per second of race time (sub_4c48d0)
const x86::reg32 InputFrames = 0x560104;    // input frames queued for the simulation, 32 per second (sub_4c6530)
const x86::reg32 Ticks       = 0x79c1a4;    // master timer ticks, 128 per second (sub_48eb90)

win32::WinApplication* s_app = nullptr;

// Render thread only.
x86::reg32 s_lastSteps = 0;
x86::reg32 s_lastInputs = 0;
x86::reg32 s_lastTicks = 0;
bool       s_haveLast = false;

void report(uint32_t intervalUs)
{
    const x86::reg32 steps = s_app->getMemory<x86::reg32>(SimSteps);
    const x86::reg32 inputs = s_app->getMemory<x86::reg32>(InputFrames);
    const x86::reg32 ticks = s_app->getMemory<x86::reg32>(Ticks);
    const int32_t newSteps = int32_t(steps - s_lastSteps);
    const int32_t newInputs = int32_t(inputs - s_lastInputs);
    const int32_t newTicks = int32_t(ticks - s_lastTicks);
    // Counters restart with every race; such an interval is skipped.
    if (s_haveLast && newSteps > 0 && newInputs >= 0 && newTicks >= 0)
    {
        const double wall = double(intervalUs) / 1000000.0;
        const double race = double(newSteps) / 64.0;
        pf::logInfo("speed: race +%.2f s in %.2f s wall (%.0f%%) | input frames %.1f/s (32) | ticks %.1f/s (128)",
                    race, wall, race * 100.0 / wall, double(newInputs) / wall, double(newTicks) / wall);
    }
    s_lastSteps = steps;
    s_lastInputs = inputs;
    s_lastTicks = ticks;
    s_haveLast = true;
}

}

void install(win32::WinApplication& app)
{
    s_app = &app;
    pf::perf::setReporter(&report);
}

}}
