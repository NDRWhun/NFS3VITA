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
std::atomic<Reporter> s_reporter{nullptr};
std::atomic<uint32_t> s_preemptCount{0};
std::atomic<uint32_t> s_preemptSum{0};
std::atomic<uint32_t> s_preemptMax{0};
std::atomic<uint32_t> s_gpuBusyCount{0};
std::atomic<uint32_t> s_gpuBusySum{0};

// Render thread only (set by init() before that thread starts).
uint32_t s_intervalStart = 0;
uint32_t s_frames = 0;
uint32_t s_drawCalls = 0;
uint32_t s_triangles = 0;
uint32_t s_peakTriangles = 0;

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

void preemptWait(uint32_t microseconds)
{
    s_preemptCount.fetch_add(1, std::memory_order_relaxed);
    s_preemptSum.fetch_add(microseconds, std::memory_order_relaxed);
    uint32_t max = s_preemptMax.load(std::memory_order_relaxed);
    while (microseconds > max && !s_preemptMax.compare_exchange_weak(max, microseconds, std::memory_order_relaxed))
    {
    }
}

void gpuBusy(uint32_t microseconds)
{
    s_gpuBusyCount.fetch_add(1, std::memory_order_relaxed);
    s_gpuBusySum.fetch_add(microseconds, std::memory_order_relaxed);
}

void framePresented(uint32_t drawCalls, uint32_t triangles)
{
    ++s_frames;
    s_drawCalls += drawCalls;
    s_triangles += triangles;
    if (triangles > s_peakTriangles)
        s_peakTriangles = triangles;
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
    const uint32_t preempts = s_preemptCount.exchange(0, std::memory_order_relaxed);
    const uint32_t preemptSum = s_preemptSum.exchange(0, std::memory_order_relaxed);
    const uint32_t preemptMax = s_preemptMax.exchange(0, std::memory_order_relaxed);
    const uint32_t gpuBusyCount = s_gpuBusyCount.exchange(0, std::memory_order_relaxed);
    const uint32_t gpuBusySum = s_gpuBusySum.exchange(0, std::memory_order_relaxed);
    logInfo("perf: %.1f fps | frame %.1f ms = game %.1f (preempted %.1f) + wait %.1f (fence %.1f) + gil %.1f"
            " | render %.1f (queue %.1f) | gpu %.1f flip %.1f busy %.1f (%u%%) | %u calls %u tris (peak %u)"
            " | preempt %.0f/s wait %.2f max %.1f ms",
            double(frames) * 1000000.0 / double(elapsed), frame, game, msPerFrame(sums[Preempted], frames), wait,
            msPerFrame(sums[FenceWait], frames), gil,
            msPerFrame(sums[Render], frames), msPerFrame(sums[QueueWait], frames),
            msPerFrame(sums[GpuLatency], frames), msPerFrame(sums[FlipWait], frames),
            gpuBusyCount ? msPerFrame(gpuBusySum, gpuBusyCount) : 0.0, gpuBusyCount * 100 / frames,
            s_drawCalls / frames, s_triangles / frames, s_peakTriangles,
            double(preempts) * 1000000.0 / double(elapsed), preempts ? msPerFrame(preemptSum, preempts) : 0.0,
            double(preemptMax) / 1000.0);
    if (Reporter reporter = s_reporter.load(std::memory_order_acquire))
        reporter(elapsed);
    s_intervalStart = time;
    s_frames = 0;
    s_drawCalls = 0;
    s_triangles = 0;
    s_peakTriangles = 0;
}

void setReporter(Reporter reporter)
{
    s_reporter.store(reporter, std::memory_order_release);
}

}}
