#include <lib/event.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <algorithm>
#include <unordered_map>

namespace win32
{

static const x86::reg32 WAIT_TIMEOUT = 0x102;

// One blocked WaitFor*Object call. Lives on the waiting thread's stack while it is registered.
struct EventWaiter
{
    Event* const*   events;
    size_t          count;
    bool            waitAll;
    bool            done;       // satisfied; result is valid
    bool            posted;     // semaphore posted and not taken yet
    x86::reg32      result;
    pf::Semaphore*  semaphore;
};

// Guards all event state, the waiter lists and the semaphore pool.
static pf::LwMutex*                 s_lock = nullptr;
static std::vector<EventWaiter*>    s_waiters;      // every registered waiter, for broadcastTerminate
static std::vector<pf::Semaphore*>  s_semaphores;   // idle wake-up semaphores, count 0
static std::unordered_map<x86::reg32, Event*> s_handles;

static void wake(EventWaiter& waiter)
{
    waiter.posted = true;
    waiter.semaphore->post();
}

static void unregister(std::vector<EventWaiter*>& list, EventWaiter* waiter)
{
    list.erase(std::find(list.begin(), list.end(), waiter));
}

Event::Event(const char* name, bool manualReset, bool initialState)
    :   m_handle(0)
    ,   m_manualReset(manualReset)
    ,   m_isSet(initialState)
{
    NFS2_USE(name);
}

Event::~Event()
{
    if (m_handle)
    {
        pf::LwLockGuard guard(*s_lock);
        s_handles.erase(m_handle);
    }
}

void Event::registerHandle(x86::reg32 handle)
{
    pf::LwLockGuard guard(*s_lock);
    m_handle = handle;
    s_handles[handle] = this;
}

bool Event::setHandle(x86::reg32 handle)
{
    pf::LwLockGuard guard(*s_lock);
    std::unordered_map<x86::reg32, Event*>::iterator it = s_handles.find(handle);
    if (it == s_handles.end())
        return false;
    it->second->m_isSet = true;
    it->second->release();
    return true;
}

bool Event::isHandleSet(x86::reg32 handle)
{
    pf::LwLockGuard guard(*s_lock);
    std::unordered_map<x86::reg32, Event*>::const_iterator it = s_handles.find(handle);
    return it != s_handles.end() && it->second->m_isSet;
}

void Event::init()
{
    s_lock = new pf::LwMutex("nfs3_events");
}

void Event::fini()
{
    for (pf::Semaphore* semaphore : s_semaphores)
        delete semaphore;
    s_semaphores.clear();
    delete s_lock;
    s_lock = nullptr;
}

void Event::broadcastTerminate()
{
    pf::LwLockGuard guard(*s_lock);
    for (EventWaiter* waiter : s_waiters)
    {
        if (!waiter->posted)
            wake(*waiter);
    }
}

// Under s_lock: completes the wait if its events allow it now, resetting the auto-reset events it takes.
bool Event::tryComplete(EventWaiter& waiter)
{
    if (waiter.waitAll)
    {
        for (size_t i = 0; i < waiter.count; ++i)
        {
            if (!waiter.events[i]->m_isSet)
                return false;
        }
        for (size_t i = 0; i < waiter.count; ++i)
        {
            if (!waiter.events[i]->m_manualReset)
                waiter.events[i]->m_isSet = false;
        }
        waiter.result = 0;
    }
    else
    {
        size_t i = 0;
        while (i < waiter.count && !waiter.events[i]->m_isSet)
            ++i;
        if (i == waiter.count)
            return false;
        if (!waiter.events[i]->m_manualReset)
            waiter.events[i]->m_isSet = false;
        waiter.result = x86::reg32(i);
    }
    waiter.done = true;
    return true;
}

// Under s_lock: wakes the waiters the signalled event satisfies, in FIFO order, until an auto-reset event is taken.
void Event::release()
{
    for (size_t i = 0; i < m_waiters.size() && m_isSet; ++i)
    {
        EventWaiter& waiter = *m_waiters[i];
        if (!waiter.done && tryComplete(waiter))
            wake(waiter);
    }
}

void Event::reset()
{
    pf::LwLockGuard guard(*s_lock);
    m_isSet = false;
}

// Releases the waiters a set would release, then resets the event.
x86::reg32 Event::pulse()
{
    pf::LwLockGuard guard(*s_lock);
    m_isSet = true;
    release();
    m_isSet = false;
    return 1;
}

x86::reg32 Event::wait(x86::CPU& cpu, Event** begin, Event** end, bool waitAll, x86::sreg32 timeout)
{
    EventWaiter waiter = { begin, size_t(end - begin), waitAll, false, false, WAIT_TIMEOUT, nullptr };
    s_lock->lock();
    if (tryComplete(waiter) || timeout == 0 || cpu.terminate)
    {
        s_lock->unlock();
        return waiter.done ? waiter.result : timeout == 0 ? WAIT_TIMEOUT : x86::reg32(-1);
    }
    if (s_semaphores.empty())
    {
        waiter.semaphore = new pf::Semaphore("nfs3_event_wait", 0);
    }
    else
    {
        waiter.semaphore = s_semaphores.back();
        s_semaphores.pop_back();
    }
    for (Event** e = begin; e < end; ++e)
        (*e)->m_waiters.push_back(&waiter);
    s_waiters.push_back(&waiter);

    const uint64_t deadline = timeout > 0 ? pf::ticksUs() + uint64_t(timeout) * 1000 : 0;
    while (!waiter.done && !cpu.terminate)
    {
        bool woken;
        if (timeout < 0)
        {
            s_lock->unlock();
            waiter.semaphore->wait();
            woken = true;
        }
        else
        {
            const uint64_t now = pf::ticksUs();
            if (now >= deadline)
                break;
            const uint64_t remaining = deadline - now;
            s_lock->unlock();
            woken = waiter.semaphore->waitForUs(uint32_t(std::min<uint64_t>(remaining, 0x7fffffff)));
        }
        s_lock->lock();
        if (woken)
            waiter.posted = false;
    }

    for (Event** e = begin; e < end; ++e)
        unregister((*e)->m_waiters, &waiter);
    unregister(s_waiters, &waiter);
    // A wake-up posted after the timeout is still pending: take it so the pooled semaphore is at 0.
    if (waiter.posted)
        waiter.semaphore->wait();
    s_semaphores.push_back(waiter.semaphore);
    s_lock->unlock();
    if (waiter.done)
        return waiter.result;
    return cpu.terminate ? x86::reg32(-1) : WAIT_TIMEOUT;
}

}
