#include <lib/mutex.h>

namespace win32
{

Mutex::Mutex(const char* name, Kind kind)
{
    const char* hostName = name && *name ? name : "nfs3_guest_mutex";
    if (kind == Kernel)
        m_kernel.reset(new pf::Mutex(hostName, true));
    else
        m_light.reset(new pf::LwMutex(hostName, true));
}

Mutex::~Mutex()
{
}

void Mutex::lock()
{
    if (m_kernel)
        m_kernel->lock();
    else
        m_light->lock();
}

void Mutex::unlock()
{
    if (m_kernel)
        m_kernel->unlock();
    else
        m_light->unlock();
}

bool Mutex::tryLock()
{
    return m_kernel ? m_kernel->tryLock() : m_light->tryLock();
}

}
