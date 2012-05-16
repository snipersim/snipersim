#include "pin_thread.h"
#include <assert.h>

PinThread::PinThread(ThreadFunc func, void *param)
   : m_thread_p(INVALID_THREADID)
   , m_func(func)
   , m_param(param)
{
}

PinThread::~PinThread()
{
}

void PinThread::run()
{
   m_thread_p = PIN_SpawnInternalThread(m_func, m_param, 32*1024*1024, NULL);
   assert(m_thread_p != INVALID_THREADID);
}

_Thread* _Thread::create(ThreadFunc func, void *param)
{
   return new PinThread(func, param);
}
