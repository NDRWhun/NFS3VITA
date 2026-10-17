#include <lib/thread.h>
#include <lib/event.h>
#include <lib/window.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>

namespace win32
{

static const uint32_t s_hostStackSize = 1024 * 1024;

static std::atomic<uint32_t> s_threadCount{1};

// Host thread id -> guest thread id. Unregistered threads report the main thread (1).
struct ThreadIdEntry
{
    int         hostId;
    x86::reg32  guestId;
};
static const int    s_maxThreadIds = 64;
static ThreadIdEntry s_threadIds[s_maxThreadIds];
static int          s_threadIdCount = 0;
static pf::LwMutex  s_threadIdMutex("nfs3_thread_ids");

static void registerCurrentThread(x86::reg32 guestId)
{
    pf::LwLockGuard guard(s_threadIdMutex);
    if (s_threadIdCount < s_maxThreadIds)
    {
        s_threadIds[s_threadIdCount].hostId = pf::Thread::currentId();
        s_threadIds[s_threadIdCount].guestId = guestId;
        s_threadIdCount++;
    }
    else
    {
        pf::logError("thread id table full");
    }
}

static void unregisterCurrentThread()
{
    pf::LwLockGuard guard(s_threadIdMutex);
    int hostId = pf::Thread::currentId();
    for (int i = 0; i < s_threadIdCount; ++i)
    {
        if (s_threadIds[i].hostId == hostId)
        {
            s_threadIds[i] = s_threadIds[--s_threadIdCount];
            return;
        }
    }
}

Thread::Shared::Shared(WinApplication* app, x86::reg32 entryPoint, x86::reg32 parameter, x86::reg32 flags)
    :   m_refCount(1)
    ,   m_app(app)
    ,   m_entryPoint(entryPoint)
    ,   m_parameter(parameter)
    ,   m_flags(flags)
    ,   m_threadId(1 + s_threadCount.fetch_add(1))
    ,   m_priority(0)
    ,   m_semaphore(nullptr)
    ,   m_thread(nullptr)
    ,   m_cpu{}
{
}

Thread::Shared::~Shared()
{
    delete m_thread;
    delete m_semaphore;
}

// Handle without a host thread (GetCurrentThread).
Thread::Thread()
    :   m_shared(new Shared(nullptr, 0, 0, 0))
{
}

Thread::Thread(const Thread& other)
    :   m_shared(other.m_shared)
{
    m_shared->m_refCount.fetch_add(1);
}

Thread::Thread(WinApplication *app, x86::reg32 method, x86::reg32 parameter, x86::reg32 flags)
    :   m_shared(new Shared(app, method, parameter, flags))
{
    m_shared->m_semaphore = new pf::Semaphore("nfs3_thread_start", (flags & 0x4) ? 0 : 1);
    m_shared->m_thread = new pf::Thread("nfs3_guest", &run, m_shared, pf::Thread::PriorityNormal, s_hostStackSize, pf::Thread::CpuAll);
    m_shared->m_thread->start();
}

Thread::~Thread()
{
    if (m_shared->m_refCount.fetch_sub(1) == 1)
    {
        terminate();
        delete m_shared;
    }
}

void Thread::resume()
{
    m_shared->m_semaphore->post();
}

void Thread::setPriority(int priority)
{
    m_shared->m_priority.store(priority, std::memory_order_relaxed);
}

x86::reg32 Thread::currentThreadId()
{
    pf::LwLockGuard guard(s_threadIdMutex);
    int hostId = pf::Thread::currentId();
    for (int i = 0; i < s_threadIdCount; ++i)
    {
        if (s_threadIds[i].hostId == hostId)
            return s_threadIds[i].guestId;
    }
    return 1;
}

// Stops the guest thread and waits for it; a second call or a handle-only object is a no-op.
void Thread::terminate()
{
    if (!m_shared->m_thread)
        return;
    m_shared->m_cpu.terminate = true;
    Event::broadcastTerminate();
    pf::logDebug("Waiting for thread 0x%x", m_shared->m_threadId);
    Window::wake();
    m_shared->m_thread->join();
    delete m_shared->m_thread;
    m_shared->m_thread = nullptr;
    delete m_shared->m_semaphore;
    m_shared->m_semaphore = nullptr;
}

x86::reg32 Thread::threadId() const
{
    return m_shared->m_threadId;
}

void Thread::sleep(x86::reg32 milliseconds)
{
    pf::sleepMs(milliseconds);
}

int Thread::run(void* data)
{
    Shared* shared = static_cast<Shared*>(data);
    registerCurrentThread(shared->m_threadId);
    shared->m_semaphore->wait();
    const bool preempts = shared->m_priority.load(std::memory_order_relaxed) > 0;
    int result = shared->m_app->runThread(shared->m_cpu, shared->m_entryPoint, shared->m_parameter, true, preempts);
    unregisterCurrentThread();
    return result;
}

}
