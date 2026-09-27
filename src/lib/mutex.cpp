#include <lib/mutex.h>

namespace win32
{

Mutex::Mutex(const char* name)
    :   m_mutex(name && *name ? name : "nfs3_guest_mutex", true)
    ,   m_name(name ? name : "")
{
}

Mutex::~Mutex()
{
}

void Mutex::lock()
{
    m_mutex.lock();
}

void Mutex::unlock()
{
    m_mutex.unlock();
}

}
