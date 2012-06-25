#include "simulator.h"
#include "core_manager.h"
#include "pthread_emu.h"
#include "thread_manager.h"
#include "performance_model.h"
#include "sync_api.h"
#include "log.h"
#include "stats.h"
#include "logmem.h"
#include "config.hpp"

#include <stdlib.h>
#include <malloc.h>
#include <errno.h>

namespace PthreadEmu {

bool pthread_stats_added = false;
const char *pthread_names[] =
{
   "pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_unlock",
   "pthread_cond_wait", "pthread_cond_signal", "pthread_cond_broadcast",
   "pthread_barrier_wait"
};
static_assert(PTHREAD_ENUM_LAST == sizeof(pthread_names) / sizeof(char*), "Not enough values in pthread_names");

struct pthread_counters_t
{
   UInt64 pthread_count[7];
   UInt64 __unused1;
   SubsecondTime pthread_total_delay_sync[7];
   SubsecondTime pthread_total_delay_mem[7];
   UInt64 pthread_mutex_lock_contended;
   UInt64 pthread_mutex_unlock_contended;
} *pthread_counters = NULL;

void pthreadCount(pthread_enum_t function, Core *core, SubsecondTime delay_sync, SubsecondTime delay_mem)
{
   pthread_counters[core->getId()].pthread_count[function]++;
   pthread_counters[core->getId()].pthread_total_delay_sync[function] += delay_sync;
   pthread_counters[core->getId()].pthread_total_delay_mem[function] += delay_mem;
}

/* Model the kernel's hash_bucket lock used in the futex syscall.
   Contended pthread_mutex_[un]lock calls should bring this address into the cache in exclusive state.
   Some mutexes may collide if the hash function maps to the same value, but let's assume this is uncommon.
   Instead, give each mutex (more or less) its own cache line. Allocate these for the real process as well.
*/
static std::unordered_map<pthread_mutex_t*, IntPtr> futex_map;
static Lock futex_map_lock;
IntPtr futexHbAddress(pthread_mutex_t *mux) {
   ScopedLock sl(futex_map_lock);
   if (futex_map.count(mux) == 0)
      futex_map[mux] = (IntPtr)memalign(64, 64);
   return futex_map[mux];
}

static Lock trace_lock;
static FILE *trace_fp = NULL;
void updateState(Core *core, state_t state, SubsecondTime delay) {
   if (trace_fp) {
      ScopedLock sl(trace_lock);
      fprintf(trace_fp, "%u %" PRIu64 " %u\n", core->getId(), (core->getPerformanceModel()->getElapsedTime() + delay).getNS(), state);
   }
}


void init()
{
   if (! pthread_stats_added) {
      UInt32 num_cores = Sim()->getConfig()->getTotalCores();
      UInt32 pthread_counters_size = sizeof(struct pthread_counters_t) * num_cores;
      __attribute__((unused)) int rc = posix_memalign((void**)&pthread_counters, 64, pthread_counters_size); // Align by cache line size to prevent thread contention
      LOG_ASSERT_ERROR (rc == 0, "posix_memalign failed to allocate memory");
      bzero(pthread_counters, pthread_counters_size);

      // Register the metrics
      for (uint32_t c = 0 ; c < num_cores ; c++ )
      {
         for (int e = PTHREAD_MUTEX_LOCK ; e < PTHREAD_ENUM_LAST ; e++ )
         {
            registerStatsMetric("pthread", c, String(pthread_names[e]) + "_count",      &(pthread_counters[c].pthread_count[e]));
            registerStatsMetric("pthread", c, String(pthread_names[e]) + "_delay_sync", &(pthread_counters[c].pthread_total_delay_sync[e]));
            registerStatsMetric("pthread", c, String(pthread_names[e]) + "_delay_mem",  &(pthread_counters[c].pthread_total_delay_mem[e]));
         }
         registerStatsMetric("pthread", c, "pthread_mutex_lock_contended", &(pthread_counters[c].pthread_mutex_lock_contended));
         registerStatsMetric("pthread", c, "pthread_mutex_unlock_contended", &(pthread_counters[c].pthread_mutex_unlock_contended));
      }

      if (Sim()->getCfg()->getBool("log/mutex_trace"))
        trace_fp = fopen(Sim()->getConfig()->formatOutputFileName("mutextrace.txt").c_str(), "w");

      pthread_stats_added = true;
   }
}


IntPtr MutexInit (pthread_mutex_t *mux, pthread_mutexattr_t *attributes)
{
   //TODO: add support for different attributes and throw warnings for unsupported attrs
   if (attributes != NULL)
   {
      char sum = 0;
      for(int i = 0; i < __SIZEOF_PTHREAD_MUTEXATTR_T; ++i)
         sum |= attributes->__size[i];
      if (sum)
         fprintf(stdout, "Warning: pthread_mutex_init() is using unsupported attributes.\n");
   }

   CarbonMutexInit((carbon_mutex_t*) mux);

   return 0;
}


IntPtr MutexLock (pthread_mutex_t *mux)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   pthread_mutex_t _mux;

   /* Model the lock cmpxchg(mux) inside the real pthread_mutex_lock/lll_lock */
   MemoryResult lat = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) mux, (char *) &_mux, sizeof (pthread_mutex_t), Core::MEM_MODELED_FENCED);

   updateState(core, STATE_WAITING);
   SubsecondTime delay = CarbonMutexLock((carbon_mutex_t*) mux, lat.latency);

   MemoryResult lat1 = makeMemoryResult(HitWhere::UNKNOWN, SubsecondTime::Zero());
   if (delay > SubsecondTime::Zero()) { /* Assume in the uncontended case, nothing (not the (system) network, nor the MCPs SyncServer) adds any delay */
      /* Model the lock addw(hb->spinlock) inside the futex_wake syscall */
      lat1 = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) futexHbAddress(mux), NULL, sizeof (UInt32), Core::MEM_MODELED_FENCED);
      pthread_counters[core->getId()].pthread_mutex_lock_contended++;
   }

   /* Delay and lat will be pushed as dynamic instructions, but have not been processed yet so we need to tell updateState to add them to core->getCycleCount(). */
   updateState(core, STATE_INREGION, delay + lat.latency + lat1.latency);

   pthreadCount(PTHREAD_MUTEX_LOCK, core, delay, lat.latency + lat1.latency);
   return 0;
}

IntPtr MutexTrylock (pthread_mutex_t *mux)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   pthread_mutex_t _mux;

   /* Model the lock cmpxchg(mux) inside the real pthread_mutex_trylock/lll_trylock */
   MemoryResult lat = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) mux, (char *) &_mux, sizeof (pthread_mutex_t), Core::MEM_MODELED_FENCED);

   updateState(core, STATE_WAITING);
   SubsecondTime res = CarbonMutexTrylock((carbon_mutex_t*) mux);
   if (res == SubsecondTime::MaxTime()) updateState(core, STATE_RUNNING, lat.latency);
   else updateState(core, STATE_INREGION, lat.latency);

   pthreadCount(PTHREAD_MUTEX_TRYLOCK, core, res == SubsecondTime::MaxTime() ? SubsecondTime::Zero() : res, lat.latency);
   return res == SubsecondTime::MaxTime() ? EBUSY : 0;
}

IntPtr MutexUnlock (pthread_mutex_t *mux)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   pthread_mutex_t _mux;

   /* Model the lock sub(mux) inside the real pthread_mutex_unlock/lll_unlock */
   MemoryResult lat = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) mux, (char *) &_mux, sizeof (pthread_mutex_t), Core::MEM_MODELED_FENCED);

   SubsecondTime delay = CarbonMutexUnlock((carbon_mutex_t*) mux, lat.latency);

   MemoryResult lat1 = makeMemoryResult(HitWhere::UNKNOWN, SubsecondTime::Zero());
   if (delay > SubsecondTime::Zero()) {
      /* Model the lock addw(hb->spinlock) inside the futex_wait syscall */
      // TODO: the latency hit for this should actually be while still holding the lock.
      //   But we can't request the latency until we've contacted the server (which already releases the lock) to tell us whether it's contended
      //   Also, no-one is currently spinning on this (and keeping the line in shared state) -- in fact, we may have even been the last ones to have used it in our matching pthread_mutex_lock call
      lat1 = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) futexHbAddress(mux), NULL, sizeof (UInt32), Core::MEM_MODELED_FENCED);
      pthread_counters[core->getId()].pthread_mutex_unlock_contended++;
   }

   updateState(core, STATE_RUNNING, delay + lat.latency + lat1.latency);

   pthreadCount(PTHREAD_MUTEX_UNLOCK, core, delay, lat.latency + lat1.latency);
   return 0;
}

IntPtr CondInit (pthread_cond_t *cond, pthread_condattr_t *attributes)
{
   //TODO: add support for different attributes and throw warnings for unsupported attrs
   if (attributes != NULL)
   {
      char sum = 0;
      for(int i = 0; i < __SIZEOF_PTHREAD_CONDATTR_T; ++i)
         sum |= attributes->__size[i];
      if (sum)
         fprintf(stdout, "Warning: pthread_cond_init() is using unsupported attributes.\n");
   }

   CarbonCondInit ((carbon_cond_t*) cond);

   return 0;
}

IntPtr CondWait (pthread_cond_t *cond, pthread_mutex_t *mutex)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   pthread_cond_t _cond;
   pthread_mutex_t _mutex;

   /* Model the locked instructions and writes inside the real pthread_cond_wait */
   MemoryResult lat2 = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) mutex, (char *) &_mutex, sizeof (pthread_mutex_t), Core::MEM_MODELED_FENCED);
   MemoryResult lat1 = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) cond, (char *) &_cond, sizeof (pthread_cond_t), Core::MEM_MODELED_TIME);

   updateState(core, STATE_WAITING);
   SubsecondTime delay = CarbonCondWait ((carbon_cond_t*) cond, (carbon_mutex_t*) mutex);
   updateState(core, STATE_RUNNING, delay + lat1.latency + lat2.latency);

   pthreadCount(PTHREAD_COND_WAIT, core, delay, lat1.latency + lat2.latency);
   return 0;
}

IntPtr CondSignal (pthread_cond_t *cond)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   pthread_cond_t _cond;

   /* Model the locked instructions and writes inside the real pthread_cond_signal */
   MemoryResult lat = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) cond, (char *) &_cond, sizeof (pthread_cond_t), Core::MEM_MODELED_FENCED);

   SubsecondTime delay = CarbonCondSignal ((carbon_cond_t*) cond);

   pthreadCount(PTHREAD_COND_SIGNAL, core, delay, lat.latency);
   return 0;
}

IntPtr CondBroadcast (pthread_cond_t *cond)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   pthread_cond_t _cond;

   /* Model the locked instructions and writes inside the real pthread_cond_broadcast */
   MemoryResult lat = core->accessMemory(Core::NONE, Core::READ_EX, (IntPtr) cond, (char *) &_cond, sizeof (pthread_cond_t), Core::MEM_MODELED_FENCED);

   SubsecondTime delay = CarbonCondBroadcast ((carbon_cond_t*) cond);

   pthreadCount(PTHREAD_COND_BROADCAST, core, delay, lat.latency);
   return 0;
}

IntPtr BarrierInit (pthread_barrier_t *barrier, pthread_barrierattr_t *attributes, unsigned count)
{
   //TODO: add support for different attributes and throw warnings for unsupported attrs
   if (attributes != NULL)
   {
      char sum = 0;
      for(int i = 0; i < __SIZEOF_PTHREAD_BARRIERATTR_T; ++i)
         sum |= attributes->__size[i];
      if (sum)
         fprintf(stdout, "Warning: pthread_barrier_init() is using unsupported attributes.\n");
   }

   carbon_barrier_t barrier_buf;

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);
   core->accessMemory (Core::NONE, Core::READ, (IntPtr) barrier, (char*) &barrier_buf, sizeof (barrier_buf));
   CarbonBarrierInit (&barrier_buf, count);
   core->accessMemory (Core::NONE, Core::WRITE, (IntPtr) barrier, (char*) &barrier_buf, sizeof (barrier_buf));

   return 0;
}

IntPtr BarrierWait (pthread_barrier_t *barrier)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   assert (core);

   carbon_barrier_t barrier_buf;

   /* Use READ_EX rather than READ since a real pthread_barrier_wait() would write to barrier, so we need the lines in M state.
      Also use MEM_MODELED_FENCED since there is a lock cmpxchg instruction in the implementation of pthread_barrier_wait */
   MemoryResult lat = core->accessMemory (Core::NONE, Core::READ_EX, (IntPtr) barrier, (char*) &barrier_buf, sizeof (barrier_buf), Core::MEM_MODELED_FENCED);

   updateState(core, STATE_WAITING);
   SubsecondTime delay = CarbonBarrierWait (&barrier_buf);
   updateState(core, STATE_RUNNING, delay + lat.latency);

   pthreadCount(PTHREAD_BARRIER_WAIT, core, delay, lat.latency);
   return 0; /* TODO: should return PTHREAD_BARRIER_SERIAL_THREAD to *one* of the threads waiting on this barrier */
}

}
