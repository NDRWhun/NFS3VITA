#include <platform/perf.h>
#include <platform/log.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <atomic>

namespace pf { namespace perf {

bool g_enabled = false;

namespace
{

const uint32_t LogIntervalUs = 5000000;

std::atomic<uint32_t> s_counters[CounterCount];

// Render thread only (set by init() before that thread starts).
uint32_t s_intervalStart = 0;
uint32_t s_frames = 0;
uint32_t s_drawCalls = 0;
uint32_t s_triangles = 0;

double msPerFrame(uint32_t microseconds, uint32_t frames)
{
    return double(microseconds) / 1000.0 / double(frames);
}

}

void init(bool enabled)
{
    g_enabled = enabled;
    s_intervalStart = now();
    logInfo("perf: timing log %s (perf_log), main thread priority %d affinity 0x%x, clocks arm %d bus %d gpu %d xbar %d MHz",
            enabled ? "on" : "off", sceKernelGetThreadCurrentPriority(),
            sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()),
            scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
            scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
}

uint32_t now()
{
    return uint32_t(sceKernelGetProcessTimeWide());
}

void add(Counter counter, uint32_t microseconds)
{
    s_counters[counter].fetch_add(microseconds, std::memory_order_relaxed);
}

void framePresented(uint32_t drawCalls, uint32_t triangles)
{
    ++s_frames;
    s_drawCalls += drawCalls;
    s_triangles += triangles;
    const uint32_t time = now();
    const uint32_t elapsed = time - s_intervalStart;
    if (elapsed < LogIntervalUs)
        return;

    uint32_t sums[CounterCount];
    for (int i = 0; i < CounterCount; ++i)
        sums[i] = s_counters[i].exchange(0, std::memory_order_relaxed);
    const uint32_t frames = s_frames;
    const double frame = msPerFrame(elapsed, frames);
    const double wait = msPerFrame(sums[SlotWait], frames);
    const double gil = msPerFrame(sums[GilWait], frames);
    const double game = frame > wait + gil ? frame - wait - gil : 0.0;
    logInfo("perf: %.1f fps | frame %.1f ms = game %.1f + wait %.1f (fence %.1f) + gil %.1f | render %.1f (queue %.1f)"
            " | gpu %.1f flip %.1f | %u calls %u tris",
            double(frames) * 1000000.0 / double(elapsed), frame, game, wait, msPerFrame(sums[FenceWait], frames), gil,
            msPerFrame(sums[Render], frames), msPerFrame(sums[QueueWait], frames),
            msPerFrame(sums[GpuLatency], frames), msPerFrame(sums[FlipWait], frames),
            s_drawCalls / frames, s_triangles / frames);
    s_intervalStart = time;
    s_frames = 0;
    s_drawCalls = 0;
    s_triangles = 0;
}

}}
