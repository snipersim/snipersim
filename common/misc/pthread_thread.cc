#include "pthread_thread.h"
#include "log.h"

PthreadThread::PthreadThread(ThreadFunc func, void *arg)
   : m_data(func, arg)
{
}

PthreadThread::~PthreadThread()
{
   // LOG_PRINT("Joining on thread: %d", m_thread);
   // pthread_join(m_thread, NULL);
   // LOG_PRINT("Joined.");
}

void *PthreadThread::spawnedThreadFunc(void *vp)
{
   FuncData *fd = (FuncData*) vp;
   fd->func(fd->arg);
   return NULL;
}

void PthreadThread::run()
{
   LOG_PRINT("Creating thread at func: %p, arg: %p", m_data.func, m_data.arg);
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
   pthread_create(&m_thread, &attr, spawnedThreadFunc, &m_data);
}

// Check if pin_thread.cc is included in the build and has
// Thread::Create defined. If so, PthreadThread is not used.
__attribute__((weak)) _Thread* _Thread::create(ThreadFunc func, void *param)
{
   return new PthreadThread(func, param);
}
