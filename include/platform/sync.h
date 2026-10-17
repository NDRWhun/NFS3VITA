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
    char   m_name[32]; // kept for error messages
};

// Lightweight mutex: locking and unlocking without contention make no system call. Unlock passes it to the first
// waiter (FIFO). The kernel keeps the object's address, so it must not move.
class LwMutex
{
public:
    explicit LwMutex(const char* name, bool recursive = false);
    ~LwMutex();

    void lock();
    void unlock();
    bool tryLock();

private:
    LwMutex(const LwMutex&) = delete;
    LwMutex& operator=(const LwMutex&) = delete;
    alignas(8) int64_t m_work[4];   // SceKernelLwMutexWork
    char   m_name[32];              // kept for error messages
};

template< typename M >
class BasicLockGuard
{
public:
    explicit BasicLockGuard(M& mutex) : m_mutex(mutex) { m_mutex.lock(); }
    ~BasicLockGuard() { m_mutex.unlock(); }
private:
    BasicLockGuard(const BasicLockGuard&) = delete;
    BasicLockGuard& operator=(const BasicLockGuard&) = delete;
    M& m_mutex;
};

typedef BasicLockGuard<Mutex>   LockGuard;
typedef BasicLockGuard<LwMutex> LwLockGuard;

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
    bool waitForUs(uint32_t microseconds); // false on timeout; 0 only polls
    bool tryWait();
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
