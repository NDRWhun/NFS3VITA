#ifndef LIB_TIMER_H_
#define LIB_TIMER_H_

#include <lib/winapp.h>
#include <winapi/types.h>
#include <cstdint>

namespace win32
{

void getSystemTime(SYSTEMTIME* systemTime);
x86::reg32 timeGetTickCount();

// One-shot multimedia timers (timeSetEvent); all callbacks run on one service thread in due order. A fired or
// killed timer id is invalid, as on Windows.
class Timer
{
public:
    // Host replacement for a guest timer callback; runs on the timer thread without the execution lock.
    typedef void (*NativeCallback)(WinApplication* app);

    // Returns the timer id, 0 after shutdown. A guest callback runs as a preempting guest thread.
    static x86::reg32 set(WinApplication* app, x86::reg32 delayMs, x86::reg32 callback, x86::reg32 parameter);
    // False if the id is not pending (fired, killed or unknown).
    static bool kill(x86::reg32 id);
    // Timers set with this guest callback run the native one instead. Before guest code runs.
    static void setNativeCallback(x86::reg32 callback, NativeCallback native);

    static void init();
    static void fini();
};

}

#endif
