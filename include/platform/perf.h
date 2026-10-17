#ifndef PLATFORM_PERF_H_
#define PLATFORM_PERF_H_

#include <cstdint>

namespace pf { namespace perf {

// Time sums in microseconds, reported per frame and reset with every log line.
enum Counter
{
    SlotWait,       // game thread waiting for a free frame slot, FenceWait included
    FenceWait,      // game thread waiting for the GPU to finish reading a frame slot
    GilWait,        // game thread re-taking the execution lock after grBufferSwap
    Render,         // render thread building and queueing a frame, QueueWait included
    QueueWait,      // render thread blocked in sceGxmDisplayQueueAddEntry
    GpuLatency,     // sceGxmEndScene until the display callback runs, frames queued ahead included
    FlipWait,       // display callback setting the frame buffer and waiting for vblank
    Preempted,      // normal-priority threads (the game thread) giving the execution lock to preempting threads
    CounterCount
};

extern bool g_enabled;

// Main thread, before any other thread starts: sets the switch and logs start-up facts.
void init(bool enabled);
inline bool enabled() { return g_enabled; }
// Process time in microseconds, wrapping; the difference of two readings is a duration.
uint32_t now();
// Any thread.
void add(Counter counter, uint32_t microseconds);
// Any thread: time a preempting thread (timer callback, raised guest priority) waited for the execution lock.
void preemptWait(uint32_t microseconds);
// Display callback: GPU time of one frame, from the later of its sceGxmEndScene and the previous frame's
// completion until its own completion; only reported for frames whose completion time is known.
void gpuBusy(uint32_t microseconds);
// Render thread, after each game frame it queued; logs one line about every 5 s.
void framePresented(uint32_t drawCalls, uint32_t triangles);
// Called on the render thread after each log line with the interval length in microseconds.
typedef void (*Reporter)(uint32_t intervalUs);
void setReporter(Reporter reporter);

}}

#endif
