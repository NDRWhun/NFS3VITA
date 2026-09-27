#ifndef PLATFORM_SYNC_H_
#define PLATFORM_SYNC_H_

#include <cstdint>
#include <psp2/types.h>

namespace pf
{

class Mutex
{
public:
    explicit Mutex(const char* name, bool recursive = true);
    ~Mutex();

    void lock();
    void unlock();
    bool tryLock();
    SceUID handle() const { return m_id; }

private:
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    SceUID m_id;
};

class LockGuard
{
public:
    explicit LockGuard(Mutex& mutex) : m_mutex(mutex) { m_mutex.lock(); }
    ~LockGuard() { m_mutex.unlock(); }
private:
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    Mutex& m_mutex;
};

// Bound to one mutex; wait() must be called with that mutex held.
class CondVar
{
public:
    CondVar(const char* name, Mutex& mutex);
    ~CondVar();

    void wait();
    bool waitFor(uint32_t milliseconds); // false on timeout
    void signal();
    void broadcast();

private:
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;
    SceUID m_id;
};

class Semaphore
{
public:
    Semaphore(const char* name, int initialCount, int maxCount = 0x7fffffff);
    ~Semaphore();

    void wait();
    bool waitFor(uint32_t milliseconds); // false on timeout
    void post(int count = 1);

private:
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    SceUID m_id;
};

class Thread
{
public:
    typedef int (*Entry)(void* argument);

    enum Priority
    {
        PriorityHigh   = 64,
        PriorityNormal = 96,
        PriorityLow    = 127
    };

    enum Cpu
    {
        CpuDefault = 0,             // inherit the process affinity
        Cpu0       = 0x00010000,
        Cpu1       = 0x00020000,
        Cpu2       = 0x00040000,
        CpuAll     = Cpu0 | Cpu1 | Cpu2
    };

    Thread(const char* name, Entry entry, void* argument,
           int priority = PriorityNormal, uint32_t stackSize = 256 * 1024, int cpuMask = CpuDefault);
    ~Thread();

    void start();
    int join(); // exit status

    static int currentId();

private:
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    static int trampoline(SceSize argumentSize, void* argument);

    SceUID  m_id;
    Entry   m_entry;
    void*   m_argument;
    bool    m_started;
};

}

#endif
