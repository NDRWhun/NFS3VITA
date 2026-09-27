#ifndef LIB_MUTEX_H_
#define LIB_MUTEX_H_

#include <lib/winapp.h>
#include <platform/sync.h>
#include <string>

namespace win32
{

// Recursive mutex exposed to the guest as CRITICAL_SECTION / HANDLE.
class Mutex : public GenericResource
{
public:
    Mutex(const char* name);
    ~Mutex();

    void lock();
    void unlock();

    x86::reg32 getResourceIndex() const { return m_resourceIndex; }

private:
    pf::Mutex   m_mutex;
    std::string m_name;
};

}

#endif
