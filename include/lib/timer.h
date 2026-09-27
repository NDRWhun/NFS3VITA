#ifndef LIB_TIMER_H_
#define LIB_TIMER_H_

#include <lib/winapp.h>
#include <winapi/types.h>
#include <cstdint>

namespace win32
{

void getSystemTime(SYSTEMTIME* systemTime);
x86::reg32 timeGetTickCount();

// One-shot timer; the callback runs guest code on the shared timer thread.
class Timer : public GenericResource
{
    friend class TimerService;
public:
    Timer(x86::reg32 delay, WinApplication* app, x86::reg32 proc, x86::reg32 param);
    ~Timer();

    void cancel();

    static void init();
    static void fini();

private:
    WinApplication* m_app;
    x86::reg32      m_callback;
    x86::reg32      m_parameter;
    uint64_t        m_dueUs;
};

}

#endif
