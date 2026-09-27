#include <lib/event.h>
#include <lib/thread.h>
#include <platform/sync.h>
#include <algorithm>

namespace win32
{

static pf::Mutex*   s_mutex = nullptr;
static pf::CondVar* s_condition = nullptr;

static const x86::sreg32 WAIT_TIMEOUT = 0x102;

static unsigned s_eventIndex = 0;

Event::Event(const char *name, bool manualReset, bool initialState)
    :   m_name(name ? name : "")
    ,   m_manualReset(manualReset)
    ,   m_isSet(initialState)
    ,   m_pulse(false)
    ,   m_id(s_eventIndex++)
{
}

Event::~Event()
{
}

void Event::init()
{
    s_mutex = new pf::Mutex("nfs3_events", false);
    s_condition = new pf::CondVar("nfs3_events_cond", *s_mutex);
}

void Event::fini()
{
    delete s_condition;
    s_condition = nullptr;
    delete s_mutex;
    s_mutex = nullptr;
}

void Event::broadcastTerminate()
{
    pf::LockGuard guard(*s_mutex);
    s_condition->broadcast();
}

void Event::set()
{
    pf::LockGuard guard(*s_mutex);
    m_pulse = false;
    m_isSet = true;
    s_condition->broadcast();
}

x86::reg32 Event::pulse()
{
    pf::LockGuard guard(*s_mutex);
    if (!m_waitingThreads.empty())
    {
        m_pulse = true;
        m_isSet = true;
        s_condition->broadcast();
    }
    return 1;
}

x86::reg32 Event::wait(x86::CPU& cpu, Event** begin, Event** end, bool waitAll, x86::sreg32 timeout)
{
    NFS2_ASSERT(!waitAll);
    pf::LockGuard guard(*s_mutex);
    x86::reg32 result = WAIT_TIMEOUT;
    x86::reg32 threadId = Thread::currentThreadId();
    for (Event** e = begin; e < end; ++e)
    {
        (*e)->m_waitingThreads.push_back(threadId);
    }
    for (;;)
    {
        for (Event** e = begin; e < end; ++e)
        {
            if ((*e)->m_isSet)
            {
                if (result == WAIT_TIMEOUT)
                    result = static_cast<x86::reg32>(e - begin);
                if (!(*e)->m_manualReset)
                    (*e)->m_isSet = false;
            }
        }

        if (result != WAIT_TIMEOUT || timeout == 0)
        {
            break;
        }
        if (cpu.terminate)
        {
            result = x86::reg32(-1);
            break;
        }
        if (timeout < 0)
        {
            s_condition->wait();
        }
        else if (!s_condition->waitFor(x86::reg32(timeout)))
        {
            break;
        }
    }
    for (Event** e = begin; e < end; ++e)
    {
        (*e)->m_waitingThreads.erase(std::remove((*e)->m_waitingThreads.begin(),
                                                 (*e)->m_waitingThreads.end(),
                                                 threadId),
                                     (*e)->m_waitingThreads.end());
        if ((*e)->m_pulse && !(*e)->m_waitingThreads.empty())
            (*e)->m_isSet = false;
    }
    return result;
}

}
