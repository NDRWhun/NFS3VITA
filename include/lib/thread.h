#ifndef LIB_THREAD_H_
#define LIB_THREAD_H_

#include <lib/winapp.h>
#include <atomic>

namespace pf
{
class Thread;
class Semaphore;
}

namespace win32
{

// Guest thread: runs recompiled code on its own host thread and virtual CPU.
// Handles created by GetCurrentThread/DuplicateHandle share the same thread state.
class Thread : public GenericResource
{
public:
    Thread();
    Thread(WinApplication* app, x86::reg32 method, x86::reg32 parameter, x86::reg32 flags);
    ~Thread();

    Thread* duplicate()
    {
        return new Thread(*this);
    }

    void resume();
    // Guest priority (THREAD_PRIORITY_*), read when the thread starts: above normal makes it preempt normal threads
    // (WinApplication::runThread). nfs3.exe sets it on suspended threads only (sub_4df7a0).
    void setPriority(int priority);
    void terminate();
    x86::reg32 threadId() const;
    static void sleep(x86::reg32 milliseconds);
    static x86::reg32 currentThreadId();

private:
    Thread(const Thread& other);
    static int run(void* parameter);

private:
    struct Shared
    {
        Shared(WinApplication* app, x86::reg32 entryPoint, x86::reg32 parameter, x86::reg32 flags);
        ~Shared();

        std::atomic<int> m_refCount;
        WinApplication*  m_app;
        x86::reg32       m_entryPoint;
        x86::reg32       m_parameter;
        x86::reg32       m_flags;
        x86::reg32       m_threadId;
        std::atomic<int> m_priority;
        pf::Semaphore*   m_semaphore;
        pf::Thread*      m_thread;
        x86::CPU         m_cpu;
    };

    Shared* m_shared;
};

}

#endif
