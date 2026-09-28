#include <platform/prof.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace pf { namespace prof {

namespace
{

const int      MaxThreads = 32;                 // guest threads registered at once
const uint32_t MaxClasses = 16;                 // thread entry points reported apart; later ones share the last
const uint32_t TableBits = 15;
const uint32_t TableSize = 1u << TableBits;     // histogram buckets
const uint32_t TableLimit = TableSize / 4 * 3;  // new keys beyond this fill are dropped
const int      TopCount = 40;                   // functions listed per report for the main thread
const int      ListCount = 6;                   // entries in the one-line lists
const char*    DumpPath = "ux0:data/nfs3vita/profile.txt";

// Histogram key: bit 31 set when the thread was blocked, bits 24-27 its class, bits 0-23 its slot value.
const uint32_t BlockedBit = 0x80000000u;
const uint32_t ClassShift = 24;
const uint32_t ClassMask = 0xf;
const uint32_t AddressMask = 0x00ffffffu;
const uint32_t EmptyKey = 0xffffffffu;          // bits 28-30 of a key are always clear

// Slot values: 0 in a thread block never written (the arena starts zeroed), host shims (import and library
// pseudo-addresses from 0x400000), then guest code up to the end of the image sections (memmap.cpp).
const x86::reg32 GuestCodeStart = 0x401000;
const x86::reg32 GuestCodeEnd = 0xa98000;
const x86::reg32 InvalidAddress = AddressMask;

// Written by the runThread wrapper, read by the sampler. cpu is published last and cleared first; reading is set
// while the sampler reads through cpu, and unregistering waits for it (the CPU may be a stack object).
struct Registration
{
    std::atomic<bool>              claimed{false};
    std::atomic<const x86::CPU*>   cpu{nullptr};
    std::atomic<int>               hostThread{0};
    std::atomic<x86::reg32>        entryPoint{0};
    std::atomic<bool>              reading{false};
};

Registration      s_threads[MaxThreads];
std::atomic<bool> s_tableFullLogged{false};

// Returns the registration entry, or -1 when the table is full.
int registerThread(const x86::CPU& cpu, x86::reg32 entryPoint)
{
    for (int i = 0; i < MaxThreads; ++i)
    {
        Registration& registration = s_threads[i];
        bool expected = false;
        if (registration.claimed.compare_exchange_strong(expected, true, std::memory_order_acquire))
        {
            registration.hostThread.store(sceKernelGetThreadId(), std::memory_order_relaxed);
            registration.entryPoint.store(entryPoint, std::memory_order_relaxed);
            registration.cpu.store(&cpu, std::memory_order_release);
            return i;
        }
    }
    if (!s_tableFullLogged.exchange(true))
        logError("prof: more than %d guest threads at once; thread sub_%06x is not sampled", MaxThreads,
                 unsigned(entryPoint));
    return -1;
}

void unregisterThread(int index)
{
    if (index < 0)
        return;
    Registration& registration = s_threads[index];
    registration.cpu.store(nullptr);
    while (registration.reading.load())
    {
        // The sampler is reading two words through the CPU pointer.
    }
    registration.claimed.store(false, std::memory_order_release);
}

// Set by start() before the sampler thread starts.
const win32::WinApplication* s_app = nullptr;
int         s_mainThread = 0;
uint32_t    s_periodUs = 1000;
uint32_t    s_reportUs = 10000000;
std::atomic<bool> s_running{false};
pf::Thread* s_sampler = nullptr;

// Sampler thread only.
struct Bucket
{
    uint32_t key;
    uint32_t interval;  // samples since the last report
    uint32_t total;     // samples up to the last report
};

struct Row
{
    uint32_t key;
    uint32_t count;
};

Bucket     s_table[TableSize];
Row        s_rows[TableSize];
uint32_t   s_used = 0;
uint32_t   s_dropped = 0;
x86::reg32 s_classEntry[MaxClasses];
bool       s_classMain[MaxClasses];
uint32_t   s_classCount = 0;
uint32_t   s_reportIndex = 0;
uint64_t   s_intervalStart = 0;
uint64_t   s_startTime = 0;
uint32_t   s_ticks = 0;
uint32_t   s_totalTicks = 0;
uint64_t   s_busyUs = 0;
uint64_t   s_mainClocks = 0;
uint64_t   s_startClocks = 0;

uint64_t now()
{
    return sceKernelGetProcessTimeWide();
}

uint64_t runClocks(int thread)
{
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    return sceKernelGetThreadInfo(thread, &info) < 0 ? 0 : info.runClocks;
}

double percent(uint64_t part, uint64_t whole)
{
    return whole ? 100.0 * double(part) / double(whole) : 0.0;
}

uint32_t classOf(x86::reg32 entryPoint, int hostThread)
{
    for (uint32_t i = 0; i < s_classCount; ++i)
    {
        if (s_classEntry[i] == entryPoint)
            return i;
    }
    if (s_classCount == MaxClasses)
        return MaxClasses - 1;
    s_classEntry[s_classCount] = entryPoint;
    s_classMain[s_classCount] = hostThread == s_mainThread;
    return s_classCount++;
}

void count(uint32_t key)
{
    for (uint32_t i = (key * 0x9e3779b1u) >> (32 - TableBits); ; i = (i + 1) & (TableSize - 1))
    {
        Bucket& bucket = s_table[i];
        if (bucket.key == key)
        {
            ++bucket.interval;
            return;
        }
        if (bucket.key == EmptyKey)
        {
            if (s_used == TableLimit)
            {
                ++s_dropped;
                return;
            }
            ++s_used;
            bucket.key = key;
            bucket.interval = 1;
            return;
        }
    }
}

// One sample of every registered guest thread: its slot value and whether it was running or ready.
void sample()
{
    for (Registration& registration : s_threads)
    {
        if (!registration.cpu.load(std::memory_order_relaxed))
            continue;
        registration.reading.store(true);
        const x86::CPU* cpu = registration.cpu.load();
        // fs is set once by runThread (cpu.init) before the thread's first guest function; 0 until then.
        const x86::reg32 fs = cpu ? *reinterpret_cast<const volatile x86::reg32*>(&cpu->efs) : 0;
        x86::reg32 address = fs ? *reinterpret_cast<volatile x86::reg32*>(win32::g_guestBase + fs + SlotOffset) : 0;
        const int hostThread = registration.hostThread.load(std::memory_order_relaxed);
        const x86::reg32 entryPoint = registration.entryPoint.load(std::memory_order_relaxed);
        registration.reading.store(false, std::memory_order_release);
        if (!fs)
            continue;
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(hostThread, &info) < 0)
            continue;
        if (address >= GuestCodeEnd)
            address = InvalidAddress;
        const bool runnable = (info.status & (SCE_THREAD_RUNNING | SCE_THREAD_READY)) != 0;
        count((runnable ? 0 : BlockedBit) | (classOf(entryPoint, hostThread) << ClassShift) | address);
    }
}

uint32_t classOfKey(uint32_t key)
{
    return (key >> ClassShift) & ClassMask;
}

// Guest code as sub_<address>, host shims by their registered name.
const char* functionName(uint32_t key, char* buffer, size_t size)
{
    const x86::reg32 address = key & AddressMask;
    if (address == 0)
        return "(starting)";
    if (address == InvalidAddress)
        return "(invalid)";
    if (address < GuestCodeStart)
    {
        if (const char* name = s_app->getMethodName(address))
            return name;
        snprintf(buffer, size, "host_%06x", unsigned(address));
        return buffer;
    }
    snprintf(buffer, size, "sub_%06x", unsigned(address));
    return buffer;
}

const char* classLabel(uint32_t cls, char* buffer, size_t size)
{
    if (s_classMain[cls])
        return "main";
    snprintf(buffer, size, "t%06x", unsigned(s_classEntry[cls]));
    return buffer;
}

// Rows with a sample count, ordered by class, then by count (highest first).
uint32_t collect(bool total)
{
    uint32_t rows = 0;
    for (const Bucket& bucket : s_table)
    {
        const uint32_t samples = total ? bucket.total : bucket.interval;
        if (bucket.key != EmptyKey && samples)
            s_rows[rows++] = Row{bucket.key, samples};
    }
    std::sort(s_rows, s_rows + rows, [](const Row& a, const Row& b) {
        const uint32_t classA = classOfKey(a.key);
        const uint32_t classB = classOfKey(b.key);
        if (classA != classB)
            return classA < classB;
        return a.count != b.count ? a.count > b.count : a.key < b.key;
    });
    return rows;
}

// Appends "name share%" for the first ListCount rows with the given state.
void appendList(char* line, size_t size, const Row* rows, uint32_t count, uint32_t samples, bool blocked)
{
    int listed = 0;
    for (uint32_t i = 0; i < count && listed < ListCount; ++i)
    {
        if (((rows[i].key & BlockedBit) != 0) != blocked)
            continue;
        char name[32];
        const size_t used = strlen(line);
        snprintf(line + used, size - used, "%s%s %.1f%%", listed ? ", " : " ",
                 functionName(rows[i].key, name, sizeof(name)), percent(rows[i].count, samples));
        ++listed;
    }
}

// Logs one class: the sample split, where it blocked, and its top functions (main) or a short list (others).
void logClass(const char* report, uint32_t cls, const Row* rows, uint32_t count)
{
    uint32_t samples = 0, guest = 0, host = 0, blocked = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const x86::reg32 address = rows[i].key & AddressMask;
        samples += rows[i].count;
        if (rows[i].key & BlockedBit)
            blocked += rows[i].count;
        else if (address >= GuestCodeStart && address != InvalidAddress)
            guest += rows[i].count;
        else if (address != 0 && address != InvalidAddress)
            host += rows[i].count;
    }
    const uint32_t other = samples - guest - host - blocked;
    const bool mainThread = s_classMain[cls];
    char line[1024];
    snprintf(line, sizeof(line),
             "prof %s %s sub_%06x: %u samples: guest %.1f%%, host %.1f%%, blocked %.1f%%, other %.1f%%; blocked in",
             report, mainThread ? "main thread, entry" : "thread", unsigned(s_classEntry[cls]), samples,
             percent(guest, samples), percent(host, samples), percent(blocked, samples), percent(other, samples));
    appendList(line, sizeof(line), rows, count, samples, true);
    if (!mainThread)
    {
        strncat(line, "; running", sizeof(line) - strlen(line) - 1);
        appendList(line, sizeof(line), rows, count, samples, false);
    }
    logInfo("%s", line);
    if (!mainThread)
        return;
    uint32_t cumulative = 0;
    int rank = 0;
    for (uint32_t i = 0; i < count && rank < TopCount; ++i)
    {
        if (rows[i].key & BlockedBit)
            continue;
        char name[32];
        cumulative += rows[i].count;
        logInfo("prof %s %2d %5.1f%% %5.1f%%  %s", report, ++rank, percent(rows[i].count, samples),
                percent(cumulative, samples), functionName(rows[i].key, name, sizeof(name)));
    }
}

void dump(const char* report, const Row* rows, uint32_t count)
{
    FILE* file = fopen(DumpPath, "a");
    if (!file)
        return;
    for (uint32_t i = 0; i < count; ++i)
    {
        char thread[16], name[32];
        fprintf(file, "%s %s %s %u %s\n", report, classLabel(classOfKey(rows[i].key), thread, sizeof(thread)),
                (rows[i].key & BlockedBit) ? "blocked" : "run", rows[i].count,
                functionName(rows[i].key, name, sizeof(name)));
    }
    fclose(file);
}

// Logs and dumps the rows of one report, class by class.
void writeReport(const char* report, bool total)
{
    const uint32_t rows = collect(total);
    for (uint32_t first = 0; first < rows; )
    {
        const uint32_t cls = classOfKey(s_rows[first].key);
        uint32_t end = first;
        while (end < rows && classOfKey(s_rows[end].key) == cls)
            ++end;
        logClass(report, cls, s_rows + first, end - first);
        first = end;
    }
    dump(report, s_rows, rows);
}

void report(uint64_t time)
{
    char name[16];
    snprintf(name, sizeof(name), "#%u", ++s_reportIndex);
    const uint64_t elapsed = time - s_intervalStart;
    const uint64_t clocks = runClocks(s_mainThread);
    logInfo("prof %s: %.1f s, %u ticks (%.2f ms apart), sampler %.1f us per tick, %u keys dropped;"
            " main thread runClocks +%llu (%.0f%% of the interval)", name, double(elapsed) / 1e6, s_ticks,
            s_ticks ? double(elapsed) / 1000.0 / s_ticks : 0.0, s_ticks ? double(s_busyUs) / s_ticks : 0.0, s_dropped,
            (unsigned long long)(clocks - s_mainClocks), percent(clocks - s_mainClocks, elapsed));
    writeReport(name, false);
    for (Bucket& bucket : s_table)
    {
        bucket.total += bucket.interval;
        bucket.interval = 0;
    }
    s_totalTicks += s_ticks;
    s_intervalStart = time;
    s_ticks = 0;
    s_busyUs = 0;
    s_mainClocks = clocks;
}

int run(void*)
{
    s_startTime = s_intervalStart = now();
    s_startClocks = s_mainClocks = runClocks(s_mainThread);
    uint64_t next = s_intervalStart;
    while (s_running.load(std::memory_order_acquire))
    {
        const uint64_t begin = now();
        sample();
        const uint64_t end = now();
        ++s_ticks;
        s_busyUs += end - begin;
        if (end - s_intervalStart >= s_reportUs)
            report(end);
        next += s_periodUs;
        const uint64_t current = now();
        if (next > current)
            sceKernelDelayThread(SceUInt(next - current));
        else
            next = current;
    }
    const uint64_t time = now();
    report(time);
    logInfo("prof total: %.1f s, %u ticks; main thread runClocks +%llu", double(time - s_startTime) / 1e6, s_totalTicks,
            (unsigned long long)(s_mainClocks - s_startClocks));
    writeReport("total", true);
    return 0;
}

}

void start(const win32::WinApplication& app)
{
    s_app = &app;
    s_mainThread = sceKernelGetThreadId();
    s_periodUs = uint32_t(std::max(Config::getInt("prof_period_us", 1000), 100));
    s_reportUs = uint32_t(std::min(std::max(Config::getInt("prof_report_s", 10), 1), 3600)) * 1000000u;
    for (Bucket& bucket : s_table)
        bucket.key = EmptyKey;
    if (FILE* file = fopen(DumpPath, "w"))
    {
        fprintf(file, "# report thread state samples function (thread: main or t<guest entry point>)\n");
        fclose(file);
    }
    logInfo("prof: sampling guest threads every %u us on cpu 2, reports every %u s here and in %s;"
            " main thread affinity 0x%x", s_periodUs, s_reportUs / 1000000u, DumpPath,
            sceKernelGetThreadCpuAffinityMask(s_mainThread));
    s_running.store(true, std::memory_order_release);
    s_sampler = new pf::Thread("nfs3_prof", &run, nullptr, pf::Thread::PriorityHigh, 64 * 1024, pf::Thread::Cpu2);
    s_sampler->start();
}

void stop()
{
    if (!s_sampler)
        return;
    s_running.store(false, std::memory_order_release);
    s_sampler->join();
    delete s_sampler;
    s_sampler = nullptr;
}

}}

// The NFS3_PROFILE link wraps WinApplication::runThread (-Wl,--wrap, cmake/nfs3_profile.cmake); all its callers
// (Application::execute, guest threads, timer callbacks) are in other objects. Each thread is sampled while it runs.
extern "C" int __real__ZN5win3214WinApplication9runThreadERN3x863CPUEjjb(win32::WinApplication* app, x86::CPU& cpu,
    x86::reg32 entryPoint, x86::reg32 parameter, bool threadLock);

extern "C" int __wrap__ZN5win3214WinApplication9runThreadERN3x863CPUEjjb(win32::WinApplication* app, x86::CPU& cpu,
    x86::reg32 entryPoint, x86::reg32 parameter, bool threadLock)
{
    const int index = pf::prof::registerThread(cpu, entryPoint);
    const int result = __real__ZN5win3214WinApplication9runThreadERN3x863CPUEjjb(app, cpu, entryPoint, parameter,
                                                                                 threadLock);
    pf::prof::unregisterThread(index);
    return result;
}
