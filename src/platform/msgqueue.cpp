#include <platform/msgqueue.h>
#include <platform/time.h>

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
    const uint64_t deadline = ticksUs() + uint64_t(timeoutMs) * 1000u;
    while (m_queue.empty())
    {
        if (timeoutMs == Infinite)
        {
            m_cond.wait();
            continue;
        }
        const uint64_t now = ticksUs();
        if (now >= deadline)
            return false;
        m_cond.waitFor(uint32_t((deadline - now + 999) / 1000));
    }
    out = m_queue.front();
    m_queue.pop_front();
    return true;
}

}
