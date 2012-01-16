#include "setlock.h"
#include <assert.h>

_SetLock::_SetLock(UInt32 core_offset, UInt32 num_sharers)
   : m_locks(num_sharers)
   , m_core_offset(core_offset)
{
   #ifdef TIME_LOCKS
   _timer = TotalTimer::getTimerByStacktrace("setlock@" + itostr(this));
   #endif
}

// Acquire exclusive access
void
_SetLock::acquire_exclusive(void)
{
   #ifdef TIME_LOCKS
   ScopedTimer tt(*_timer);
   #endif

   for(std::vector<PersetLock>::iterator it = m_locks.begin(); it != m_locks.end(); ++it)
      (*it).acquire();
}

// Release exclusive access
void
_SetLock::release_exclusive(void)
{
   for(std::vector<PersetLock>::iterator it = m_locks.begin(); it != m_locks.end(); ++it)
      (*it).release();
}

// Acquire shared access
void
_SetLock::acquire_shared(UInt32 core_id)
{
   #ifdef TIME_LOCKS
   ScopedTimer tt(*_timer);
   #endif

   assert(core_id >= m_core_offset);
   assert(core_id < m_core_offset + m_locks.size());
   m_locks.at(core_id - m_core_offset).acquire();
}

// Release shared access
void
_SetLock::release_shared(UInt32 core_id)
{
   m_locks.at(core_id - m_core_offset).release();
}

void
_SetLock::upgrade(UInt32 core_id)
{
   release_shared(core_id);
   acquire_exclusive();
}

void
_SetLock::downgrade(UInt32 core_id)
{
   for(unsigned int i = 0; i < m_locks.size(); ++i)
      if (i != (core_id - m_core_offset))
         m_locks.at(i).release();
}
