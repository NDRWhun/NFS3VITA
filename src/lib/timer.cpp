#include <lib/timer.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <psp2/rtc.h>
#include <vector>

namespace win32
{

namespace
{

struct PendingTimer
{
    x86::reg32              id;
    uint64_t                dueUs;
    WinApplication*         app;
    x86::reg32              callback;
    x86::reg32              parameter;
    Timer::NativeCallback   native;
};

struct NativeEntry
{
    x86::reg32              callback;
    Timer::NativeCallback   native;
};

// Written before guest code runs, read-only afterwards.
std::vector<NativeEntry> s_natives;

Timer::NativeCallback findNative(x86::reg32 callback)
{
    for (const NativeEntry& entry : s_natives)
    {
        if (entry.callback == callback)
            return entry.native;
    }
    return nullptr;
}

}

// Single service thread firing pending timers in due order.
class TimerService
{
public:
    TimerService()
        :   m_mutex("nfs3_timers", false)
        ,   m_cond("nfs3_timers_cond", m_mutex)
        ,   m_running(true)
        ,   m_lastId(0)
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

    x86::reg32 add(const PendingTimer& timer)
    {
        pf::LockGuard guard(m_mutex);
        if (++m_lastId == 0)
            ++m_lastId;
        m_pending.push_back(timer);
        m_pending.back().id = m_lastId;
        m_cond.signal();
        return m_lastId;
    }

    bool remove(x86::reg32 id)
    {
        pf::LockGuard guard(m_mutex);
        for (std::vector<PendingTimer>::iterator it = m_pending.begin(); it != m_pending.end(); ++it)
        {
            if (it->id == id)
            {
                m_pending.erase(it);
                return true;
            }
        }
        return false;
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
            std::vector<PendingTimer>::iterator next = m_pending.begin();
            for (std::vector<PendingTimer>::iterator it = m_pending.begin() + 1; it != m_pending.end(); ++it)
            {
                if (it->dueUs < next->dueUs)
                    next = it;
            }
            const uint64_t now = pf::ticksUs();
            if (next->dueUs > now)
            {
                const uint64_t waitMs = (next->dueUs - now + 999) / 1000;
                m_cond.waitFor(uint32_t(waitMs));
                continue;
            }
            // The id is invalid from here on: timeKillEvent of a firing timer finds nothing.
            const PendingTimer timer = *next;
            m_pending.erase(next);
            m_mutex.unlock();
            fire(timer);
            m_mutex.lock();
        }
        m_mutex.unlock();
    }

    static void fire(const PendingTimer& timer)
    {
        if (timer.native)
        {
            timer.native(timer.app);
            return;
        }
        // winmm runs timer callbacks on a time-critical thread: they preempt the game thread.
        alignas(16) x86::CPU cpu{};
        timer.app->runThread(cpu, timer.callback, timer.parameter, true, true);
    }

private:
    pf::Mutex                   m_mutex;
    pf::CondVar                 m_cond;
    bool                        m_running;
    x86::reg32                  m_lastId;
    std::vector<PendingTimer>   m_pending;
    pf::Thread                  m_thread;
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

x86::reg32 Timer::set(WinApplication* app, x86::reg32 delayMs, x86::reg32 callback, x86::reg32 parameter)
{
    if (!s_service)
        return 0;
    const PendingTimer timer = { 0, pf::ticksUs() + uint64_t(delayMs) * 1000, app, callback, parameter,
                                 findNative(callback) };
    return s_service->add(timer);
}

bool Timer::kill(x86::reg32 id)
{
    return s_service && s_service->remove(id);
}

void Timer::setNativeCallback(x86::reg32 callback, NativeCallback native)
{
    s_natives.push_back(NativeEntry{ callback, native });
}

}
