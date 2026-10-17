#include <platform/sync.h>
#include <platform/log.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/error.h>
#include <cstring>

namespace pf
{

Mutex::Mutex(const char* name, bool recursive)
    :   m_id(sceKernelCreateMutex(name, recursive ? SCE_KERNEL_MUTEX_ATTR_RECURSIVE : 0, 0, nullptr))
{
    strncpy(m_name, name, sizeof(m_name) - 1);
    m_name[sizeof(m_name) - 1] = 0;
    if (m_id < 0)
        fatal("sceKernelCreateMutex(%s) failed: 0x%08x", name, m_id);
}

Mutex::~Mutex()
{
    sceKernelDeleteMutex(m_id);
    m_id = -1;
}

void Mutex::lock()
{
    int result = sceKernelLockMutex(m_id, 1, nullptr);
    if (result < 0)
        logError("sceKernelLockMutex(%s) failed: 0x%08x", m_name, result);
}

void Mutex::unlock()
{
    int result = sceKernelUnlockMutex(m_id, 1);
    if (result < 0)
        logError("sceKernelUnlockMutex(%s) failed: 0x%08x", m_name, result);
}

bool Mutex::tryLock()
{
    return sceKernelTryLockMutex(m_id, 1) == 0;
}

static SceKernelLwMutexWork* lwWork(int64_t* work)
{
    static_assert(sizeof(SceKernelLwMutexWork) == 4 * sizeof(int64_t), "LwMutex work area size");
    return reinterpret_cast<SceKernelLwMutexWork*>(work);
}

LwMutex::LwMutex(const char* name, bool recursive)
{
    strncpy(m_name, name, sizeof(m_name) - 1);
    m_name[sizeof(m_name) - 1] = 0;
    const int result = sceKernelCreateLwMutex(lwWork(m_work), m_name, recursive ? SCE_KERNEL_MUTEX_ATTR_RECURSIVE : 0, 0,
                                              nullptr);
    if (result < 0)
        fatal("sceKernelCreateLwMutex(%s) failed: 0x%08x", name, result);
}

LwMutex::~LwMutex()
{
    sceKernelDeleteLwMutex(lwWork(m_work));
}

void LwMutex::lock()
{
    const int result = sceKernelLockLwMutex(lwWork(m_work), 1, nullptr);
    if (result < 0)
        logError("sceKernelLockLwMutex(%s) failed: 0x%08x", m_name, result);
}

void LwMutex::unlock()
{
    const int result = sceKernelUnlockLwMutex(lwWork(m_work), 1);
    if (result < 0)
        logError("sceKernelUnlockLwMutex(%s) failed: 0x%08x", m_name, result);
}

bool LwMutex::tryLock()
{
    return sceKernelTryLockLwMutex(lwWork(m_work), 1) == 0;
}

CondVar::CondVar(const char* name, Mutex& mutex)
    :   m_id(sceKernelCreateCond(name, 0, mutex.handle(), nullptr))
{
    if (m_id < 0)
        fatal("sceKernelCreateCond(%s) failed: 0x%08x", name, m_id);
}

CondVar::~CondVar()
{
    sceKernelDeleteCond(m_id);
}

void CondVar::wait()
{
    sceKernelWaitCond(m_id, nullptr);
}

bool CondVar::waitFor(uint32_t milliseconds)
{
    unsigned int timeout = milliseconds * 1000u;
    if (timeout == 0)
        timeout = 1;
    return sceKernelWaitCond(m_id, &timeout) == 0;
}

void CondVar::signal()
{
    sceKernelSignalCond(m_id);
}

void CondVar::broadcast()
{
    sceKernelSignalCondAll(m_id);
}

Semaphore::Semaphore(const char* name, int initialCount, int maxCount)
    :   m_id(sceKernelCreateSema(name, 0, initialCount, maxCount, nullptr))
{
    if (m_id < 0)
        fatal("sceKernelCreateSema(%s) failed: 0x%08x", name, m_id);
}

Semaphore::~Semaphore()
{
    sceKernelDeleteSema(m_id);
}

void Semaphore::wait()
{
    sceKernelWaitSema(m_id, 1, nullptr);
}

bool Semaphore::waitFor(uint32_t milliseconds)
{
    SceUInt timeout = milliseconds * 1000u;
    if (timeout == 0)
        timeout = 1;
    return sceKernelWaitSema(m_id, 1, &timeout) == 0;
}

bool Semaphore::waitForUs(uint32_t microseconds)
{
    if (microseconds == 0)
        return tryWait();
    SceUInt timeout = microseconds;
    return sceKernelWaitSema(m_id, 1, &timeout) == 0;
}

bool Semaphore::tryWait()
{
    return sceKernelPollSema(m_id, 1) == 0;
}

void Semaphore::post(int count)
{
    int result = sceKernelSignalSema(m_id, count);
    if (result < 0)
        logError("sceKernelSignalSema failed: 0x%08x", result);
}

Thread::Thread(const char* name, Entry entry, void* argument, int priority, uint32_t stackSize, int cpuMask)
    :   m_id(sceKernelCreateThread(name, &trampoline, priority, stackSize, 0, cpuMask, nullptr))
    ,   m_entry(entry)
    ,   m_argument(argument)
    ,   m_started(false)
{
    if (m_id < 0)
        fatal("sceKernelCreateThread(%s) failed: 0x%08x", name, m_id);
}

Thread::~Thread()
{
    if (m_started)
        join();
    sceKernelDeleteThread(m_id);
}

void Thread::start()
{
    Thread* self = this;
    m_started = true;
    int result = sceKernelStartThread(m_id, sizeof(self), &self);
    if (result < 0)
        fatal("sceKernelStartThread failed: 0x%08x", result);
}

int Thread::join()
{
    int status = 0;
    if (m_started)
    {
        sceKernelWaitThreadEnd(m_id, &status, nullptr);
        m_started = false;
    }
    return status;
}

int Thread::currentId()
{
    return sceKernelGetThreadId();
}

int Thread::trampoline(SceSize argumentSize, void* argument)
{
    (void)argumentSize;
    Thread* self = *static_cast<Thread**>(argument);
    return self->m_entry(self->m_argument);
}

}
