#ifndef LIB_EVENT_H_
#define LIB_EVENT_H_
#include <lib/winapp.h>
#include <vector>

namespace win32
{

struct EventWaiter;

// Win32 event. A set or pulse wakes only the threads waiting on this event; an auto-reset event goes to the first
// waiter it satisfies (FIFO), as on Windows.
class Event : public GenericResource
{
public:
    Event(const char* name, bool manualReset, bool initialState);
    ~Event();

    void reset();
    x86::reg32 pulse();
    // Makes the event reachable by its handle from any thread (CreateEventA); the destructor removes it.
    void registerHandle(x86::reg32 handle);
    // SetEvent by handle, safe against a concurrent CloseHandle; false if no event has the handle.
    static bool setHandle(x86::reg32 handle);
    // False if no event has the handle.
    static bool isHandleSet(x86::reg32 handle);
    // WaitForMultipleObjects on event handles: index of the signalled event (0 when waitAll), WAIT_TIMEOUT, or
    // WAIT_FAILED (-1) when a handle is not an event or the thread terminates. timeout in milliseconds, negative for
    // infinite.
    static x86::reg32 wait(x86::CPU& cpu, const x86::reg32* handles, size_t count, bool waitAll, x86::sreg32 timeout);
    // Wakes every waiting thread so it checks its terminate flag.
    static void broadcastTerminate();

    static void init();
    static void fini();

private:
    void release();
    static bool tryComplete(EventWaiter& waiter);

    std::vector<EventWaiter*>   m_waiters;  // FIFO
    x86::reg32                  m_handle;   // 0 until registered
    bool                        m_manualReset;
    bool                        m_isSet;
};

}

#endif /*!EVENT_H_*/
