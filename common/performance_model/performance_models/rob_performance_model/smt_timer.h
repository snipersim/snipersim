/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#ifndef __SMT_TIMER_T
#define __SMT_TIMER_T

#include "hooks_manager.h"
#include "cond.h"
#include "boost/tuple/tuple.hpp"

class Core;
class PerformanceModel;
class DynamicMicroOp;

class SmtTimer
{
protected:
   class SmtThread {
      public:
         Core* const core;
         const PerformanceModel* const perf;
         const Thread* thread;

         bool in_wakeup;
         bool running;
         bool in_barrier;           // true when thread is waiting on cond
         ConditionVariable cond;    // thread waits here when it has too many outstanding pre-ROB instructions

         SmtThread(Core *core, PerformanceModel *perf);
         ~SmtThread();
   };

   typedef uint8_t smtthread_id_t;
   #define INVALID_SMTTHREAD_ID ((smtthread_id_t) -1)

   const uint64_t m_num_threads;
   std::vector<SmtThread *> m_threads;

   bool in_sync;
   bool enabled;
   smtthread_id_t execute_thread;

   virtual void initializeThread(smtthread_id_t thread_num) = 0;
   virtual uint64_t threadNumSurplusInstructions(smtthread_id_t thread_num) = 0;
   virtual bool threadHasEnoughInstructions(smtthread_id_t thread_num) = 0;
   virtual void notifyNumActiveThreadsChange() {}

   virtual void pushInstructions(smtthread_id_t thread_id, const std::vector<DynamicMicroOp*>& insts) = 0;
   virtual boost::tuple<uint64_t,SubsecondTime> returnLatency(smtthread_id_t thread_id) = 0;
   virtual void execute() = 0;

   char getStateStr(smtthread_id_t thread_num);

private:
   static SInt64 hookRoiBegin(UInt64 object, UInt64 argument) {
      ((SmtTimer*)object)->roiBegin(); return 0;
   }
   static SInt64 hookThreadStart(UInt64 object, UInt64 argument) {
      ((SmtTimer*)object)->threadStart((HooksManager::ThreadTime*)argument); return 0;
   }
   static SInt64 hookThreadExit(UInt64 object, UInt64 argument) {
      ((SmtTimer*)object)->threadExit((HooksManager::ThreadTime*)argument); return 0;
   }
   static SInt64 hookThreadStall(UInt64 object, UInt64 argument) {
      ((SmtTimer*)object)->threadStall((HooksManager::ThreadStall*)argument); return 0;
   }
   static SInt64 hookThreadResume(UInt64 object, UInt64 argument) {
      ((SmtTimer*)object)->threadResume((HooksManager::ThreadResume*)argument); return 0;
   }
   static SInt64 hookThreadMigrate(UInt64 object, UInt64 argument) {
      ((SmtTimer*)object)->threadMigrate((HooksManager::ThreadMigrate*)argument); return 0;
   }
   void roiBegin();
   void threadStart(HooksManager::ThreadTime *argument);
   void threadExit(HooksManager::ThreadTime *argument);
   void threadStall(HooksManager::ThreadStall *argument);
   void threadResume(HooksManager::ThreadResume *argument);
   void threadMigrate(HooksManager::ThreadMigrate *argument);

   smtthread_id_t findSmtThreadFromThread(thread_id_t thread_id);
   smtthread_id_t findSmtThreadFromCore(core_id_t core_id);

   bool barrier(smtthread_id_t thread_id);
   bool barrierRelease(bool release_all, smtthread_id_t thread_id);
   bool isBarrierReached();
   void signalBarrier();

public:
   Lock m_lock;

   SmtTimer(uint64_t num_threads);
   virtual ~SmtTimer();

   UInt8 registerThread(Core *core, PerformanceModel *perf);

   void simulate(smtthread_id_t thread_id);
   virtual void synchronize(smtthread_id_t thread_id, SubsecondTime time) = 0;
   void enable();
   void disable();
};
#endif // __SMT_TIMER_T
