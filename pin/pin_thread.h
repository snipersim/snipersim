#ifndef PIN_THREAD_H
#define PIN_THREAD_H

#include "thread.h"
#include "pin.H"

class PinThread : public Thread
{
public:
   PinThread(ThreadFunc func, void *param);
   ~PinThread();
   void run();

private:
   static const int STACK_SIZE=65536;

   THREADID m_thread_p;
   Thread::ThreadFunc m_func;
   void *m_param;
};

#endif // PIN_THREAD_H
