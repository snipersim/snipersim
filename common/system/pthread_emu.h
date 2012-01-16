#ifndef PTHREAD_EMU_H
#define PTHREAD_EMU_H

#include "fixed_types.h"
#include "subsecond_time.h"
class Core;

#include <pthread.h>

namespace PthreadEmu {

enum pthread_enum_t
{
   PTHREAD_MUTEX_LOCK = 0, PTHREAD_MUTEX_TRYLOCK, PTHREAD_MUTEX_UNLOCK,
   PTHREAD_COND_WAIT,      PTHREAD_COND_SIGNAL,   PTHREAD_COND_BROADCAST,
   PTHREAD_BARRIER_WAIT,   PTHREAD_ENUM_LAST
};

enum state_t {
   STATE_STOPPED, STATE_RUNNING, STATE_WAITING, STATE_INREGION, STATE_SEPARATOR, STATE_MAX, STATE_BY_RETURN
};

void init();
void pthreadCount(pthread_enum_t function, Core *core, SubsecondTime delay_sync, SubsecondTime delay_mem);
void updateState(Core *core, state_t state, SubsecondTime delay = SubsecondTime::Zero());

IntPtr MutexInit (pthread_mutex_t *mux, pthread_mutexattr_t *attributes);
IntPtr MutexLock (pthread_mutex_t *mux);
IntPtr MutexTrylock (pthread_mutex_t *mux);
IntPtr MutexUnlock (pthread_mutex_t *mux);
IntPtr CondInit (pthread_cond_t *cond, pthread_condattr_t *attributes);
IntPtr CondWait (pthread_cond_t *cond, pthread_mutex_t *mutex);
IntPtr CondSignal (pthread_cond_t *cond);
IntPtr CondBroadcast (pthread_cond_t *cond);
IntPtr BarrierInit (pthread_barrier_t *barrier, pthread_barrierattr_t *attributes, unsigned count);
IntPtr BarrierWait (pthread_barrier_t *barrier);

}

#endif // PTHREAD_EMU_H
