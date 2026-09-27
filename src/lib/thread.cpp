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
static pf::Mutex    s_threadIdMutex("nfs3_thread_ids", false);

static void registerCurrentThread(x86::reg32 guestId)
{
    pf::LockGuard guard(s_threadIdMutex);
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
    pf::LockGuard guard(s_threadIdMutex);
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

Thread::Data::Data(WinApplication* app, x86::reg32 entryPoint, x86::reg32 parameter, x86::reg32 flags)
    :   m_app(app)
    ,   m_entryPoint(entryPoint)
    ,   m_parameter(parameter)
    ,   m_flags(flags)
    ,   m_threadId(1 + s_threadCount.fetch_add(1))
{
}

Thread::Thread()
    :   m_refCount(new std::atomic<int>(1))
    ,   m_semaphore(nullptr)
    ,   m_data(nullptr, 0, 0, 0)
    ,   m_thread(nullptr)
    ,   m_cpu{}
{
}

Thread::Thread(const Thread& other)
    :   m_refCount(other.m_refCount)
    ,   m_semaphore(other.m_semaphore)
    ,   m_data(nullptr, 0, 0, 0)
    ,   m_thread(other.m_thread)
    ,   m_cpu{}
{
    m_refCount->fetch_add(1);
}

Thread::Thread(WinApplication *app, x86::reg32 method, x86::reg32 parameter, x86::reg32 flags)
    :   m_refCount(new std::atomic<int>(1))
    ,   m_semaphore(new pf::Semaphore("nfs3_thread_start", (flags & 0x4) ? 0 : 1))
    ,   m_data(app, method, parameter, flags)
    ,   m_thread(new pf::Thread("nfs3_guest", &run, this, pf::Thread::PriorityNormal, s_hostStackSize, pf::Thread::CpuAll))
    ,   m_cpu{}
{
    m_thread->start();
}

Thread::~Thread()
{
    if (m_refCount->fetch_sub(1) == 1)
    {
        delete m_refCount;
        if (m_thread)
        {
            terminate();
        }
    }
}

void Thread::resume()
{
    m_semaphore->post();
}

x86::reg32 Thread::currentThreadId()
{
    pf::LockGuard guard(s_threadIdMutex);
    int hostId = pf::Thread::currentId();
    for (int i = 0; i < s_threadIdCount; ++i)
    {
        if (s_threadIds[i].hostId == hostId)
            return s_threadIds[i].guestId;
    }
    return 1;
}

void Thread::terminate()
{
    m_cpu.terminate = true;
    Event::broadcastTerminate();
    pf::logDebug("Waiting for thread 0x%x", m_data.m_threadId);
    Window::wake();
    m_thread->join();
    delete m_thread;
    m_thread = nullptr;
    delete m_semaphore;
    m_semaphore = nullptr;
}

x86::reg32 Thread::threadId() const
{
    return m_data.m_threadId;
}

void Thread::sleep(x86::reg32 milliseconds)
{
    pf::sleepMs(milliseconds);
}

int Thread::run(void* data)
{
    Thread* thread = static_cast<Thread*>(data);
    registerCurrentThread(thread->m_data.m_threadId);
    thread->m_semaphore->wait();
    int result = thread->m_data.m_app->runThread(thread->m_cpu, thread->m_data.m_entryPoint, thread->m_data.m_parameter);
    unregisterCurrentThread();
    return result;
}

}
