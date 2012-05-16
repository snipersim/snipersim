#ifndef PTHREAD_THREAD_H
#define PTHREAD_THREAD_H

#include "_thread.h"
#include <pthread.h>

class PthreadThread : public _Thread
{
public:
   PthreadThread(ThreadFunc func, void *param);
   ~PthreadThread();
   void run();

private:
   static void *spawnedThreadFunc(void *);

   struct FuncData
   {
      ThreadFunc func;
      void *arg;

      FuncData(ThreadFunc f, void *a)
         : func(f)
         , arg(a)
      { }
   };

   FuncData m_data;
   pthread_t m_thread;
};

#endif // PTHREAD_THREAD_H
