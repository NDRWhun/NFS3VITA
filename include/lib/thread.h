#ifndef LIB_THREAD_H_
#define LIB_THREAD_H_

#include <lib/winapp.h>
#include <atomic>
#include <vector>

namespace pf
{
class Thread;
class Semaphore;
}

namespace win32
{

// Guest thread: runs recompiled code on its own host thread and virtual CPU.
// Handles created by DuplicateHandle share the same thread state. As on Windows, closing the last handle does not
// stop a running thread; terminateAll stops the threads still running at shutdown.
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
    // Stops the guest thread and waits for it; a no-op for a handle-only object or a thread already joined.
    void terminate();
    x86::reg32 threadId() const;
    static void sleep(x86::reg32 milliseconds);
    static x86::reg32 currentThreadId();
    // Stops and joins every guest thread, with or without a handle.
    static void terminateAll();

private:
    struct Shared;

    Thread(const Thread& other);
    static int run(void* parameter);
    static void release(Shared* shared);
    static void join(Shared* shared);
    static void reapFinished();

    static std::vector<Shared*> s_live;     // states whose host thread is not joined yet

private:
    struct Shared
    {
        Shared(WinApplication* app, x86::reg32 entryPoint, x86::reg32 parameter, x86::reg32 flags);
        ~Shared();

        std::atomic<int>  m_refCount;   // handles, plus one while the host thread is not joined
        WinApplication*   m_app;
        x86::reg32        m_entryPoint;
        x86::reg32        m_parameter;
        x86::reg32        m_flags;
        x86::reg32        m_threadId;
        std::atomic<int>  m_priority;
        std::atomic<bool> m_stop;       // set before a thread that has not started is released
        std::atomic<bool> m_finished;   // the host thread has returned from run
        pf::Semaphore*    m_semaphore;
        pf::Thread*       m_thread;
        x86::CPU          m_cpu;
    };

    Shared* m_shared;
};

}

#endif
