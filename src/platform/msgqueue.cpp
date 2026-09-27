#include <platform/msgqueue.h>

namespace pf
{

MessageQueue::MessageQueue()
    :   m_mutex("nfs3_msgqueue", false)
    ,   m_cond("nfs3_msgqueue_cond", m_mutex)
{
}

void MessageQueue::push(const Message& message)
{
    LockGuard guard(m_mutex);
    m_queue.push_back(message);
    m_cond.signal();
}

bool MessageQueue::wait(Message& out, uint32_t timeoutMs)
{
    LockGuard guard(m_mutex);
    while (m_queue.empty())
    {
        if (timeoutMs == Infinite)
        {
            m_cond.wait();
        }
        else if (!m_cond.waitFor(timeoutMs))
        {
            if (m_queue.empty())
                return false;
        }
    }
    out = m_queue.front();
    m_queue.pop_front();
    return true;
}

}
