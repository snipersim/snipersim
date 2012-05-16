#ifndef CORE_THREAD_H
#define CORE_THREAD_H

#include "_thread.h"
#include "fixed_types.h"
#include "network.h"

class CoreThread : public Runnable
{
public:
   CoreThread();
   ~CoreThread();

   void spawn();

private:
   void run();

   static void terminateFunc(void *vp, NetPacket pkt);

   _Thread *m_thread;
};

#endif // CORE_THREAD_H
