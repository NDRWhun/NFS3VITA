#include <lib/timer.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <psp2/rtc.h>
#include <algorithm>
#include <vector>

namespace win32
{

// Single service thread firing pending timers in due order.
class TimerService
{
public:
    TimerService()
        :   m_mutex("nfs3_timers", false)
        ,   m_cond("nfs3_timers_cond", m_mutex)
        ,   m_running(true)
        ,   m_thread("nfs3_timer", &threadEntry, this, pf::Thread::PriorityNormal, 1024 * 1024, pf::Thread::CpuAll)
    {
        m_thread.start();
    }

    ~TimerService()
    {
        {
            pf::LockGuard guard(m_mutex);
            m_running = false;
            m_cond.signal();
        }
        m_thread.join();
    }

    void add(Timer* timer)
    {
        pf::LockGuard guard(m_mutex);
        m_pending.push_back(timer);
        m_cond.signal();
    }

    void remove(Timer* timer)
    {
        pf::LockGuard guard(m_mutex);
        m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), timer), m_pending.end());
    }

private:
    static int threadEntry(void* self)
    {
        static_cast<TimerService*>(self)->run();
        return 0;
    }

    void run()
    {
        m_mutex.lock();
        while (m_running)
        {
            if (m_pending.empty())
            {
                m_cond.wait();
                continue;
            }
            std::vector<Timer*>::iterator next = m_pending.begin();
            for (std::vector<Timer*>::iterator it = m_pending.begin() + 1; it != m_pending.end(); ++it)
            {
                if ((*it)->m_dueUs < (*next)->m_dueUs)
                    next = it;
            }
            uint64_t now = pf::ticksUs();
            if ((*next)->m_dueUs > now)
            {
                uint64_t waitMs = ((*next)->m_dueUs - now + 999) / 1000;
                m_cond.waitFor(uint32_t(waitMs));
                continue;
            }
            // Copy what the callback needs; the Timer may be freed while it runs.
            WinApplication* app = (*next)->m_app;
            x86::reg32 callback = (*next)->m_callback;
            x86::reg32 parameter = (*next)->m_parameter;
            m_pending.erase(next);
            m_mutex.unlock();
            fire(app, callback, parameter);
            m_mutex.lock();
        }
        m_mutex.unlock();
    }

    static void fire(WinApplication* app, x86::reg32 callback, x86::reg32 parameter)
    {
        alignas(16) x86::CPU cpu{};
        app->runThread(cpu, callback, parameter);
    }

private:
    pf::Mutex           m_mutex;
    pf::CondVar         m_cond;
    bool                m_running;
    std::vector<Timer*> m_pending;
    pf::Thread          m_thread;
};

static TimerService* s_service = nullptr;

void getSystemTime(SYSTEMTIME* systemTime)
{
    SceDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    systemTime->wYear = WORD(now.year);
    systemTime->wMonth = WORD(now.month);
    systemTime->wDayOfWeek = WORD(sceRtcGetDayOfWeek(now.year, now.month, now.day));
    systemTime->wDay = WORD(now.day);
    systemTime->wHour = WORD(now.hour);
    systemTime->wMinute = WORD(now.minute);
    systemTime->wSecond = WORD(now.second);
    systemTime->wMilliseconds = WORD(now.microsecond / 1000);
}

x86::reg32 timeGetTickCount()
{
    return pf::ticksMs();
}

void Timer::init()
{
    if (!s_service)
        s_service = new TimerService();
}

void Timer::fini()
{
    delete s_service;
    s_service = nullptr;
}

Timer::Timer(x86::reg32 delay, WinApplication* app, x86::reg32 proc, x86::reg32 param)
    :   m_app(app)
    ,   m_callback(proc)
    ,   m_parameter(param)
    ,   m_dueUs(pf::ticksUs() + uint64_t(delay) * 1000)
{
    s_service->add(this);
}

Timer::~Timer()
{
    cancel();
}

void Timer::cancel()
{
    s_service->remove(this);
}

}
