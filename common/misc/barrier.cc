#include "barrier.h"
#include "log.h"

Barrier::Barrier(int count)
   : m_count(count)
   , m_arrived(0)
   , m_leaving(0)
   , m_lock()
   , m_cond()
{
}

Barrier::~Barrier()
{
}

void Barrier::wait()
{
   while((volatile int)m_leaving > 0)
      sched_yield(); // Not everyone has left, wait a bit

   m_lock.acquire();
   ++m_arrived;

   if (m_arrived == m_count)
   {
      m_arrived = 0;
      m_leaving = m_count - 1;
      m_lock.release();
      m_cond.broadcast();
   }
   else
   {
      m_cond.wait(m_lock);
      --m_leaving;
      m_lock.release();
   }
}
