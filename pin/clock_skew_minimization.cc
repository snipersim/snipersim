#include "clock_skew_minimization.h"
#include "simulator.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "performance_model.h"
#include "core.h"
#include "thread.h"
#include "clock_skew_minimization_object.h"
#include "utils.h"
#include "subsecond_time.h"
#include "inst_mode_macros.h"
#include "local_storage.h"

#include <stdlib.h>

#define SKEW_MAX_CPUS 64
#define SKEW_BINS 20

UInt64 skew_hist[SKEW_MAX_CPUS][SKEW_BINS] = {{ 0 }};
UInt64 skew_sum[SKEW_MAX_CPUS] = { 0 };
UInt64 skew_count[SKEW_MAX_CPUS] = { 0 };
UInt64 skew_numcores = 0;
bool skew_report_added = false;
bool skew_do = false;

bool appDebug_enabled = false;

SubsecondTime skew_get_localtime(core_id_t core_id) {
   return Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel()->getElapsedTime();
}

void skew_record(core_id_t core_id) {
   LOG_ASSERT_ERROR(core_id < SKEW_MAX_CPUS, "Increase SKEW_MAX_CPUS");

   SubsecondTime t0 = skew_get_localtime(0), t1 = skew_get_localtime(core_id);
   SubsecondTime diff_fs = (t1 > t0) ? (t1 - t0) : (t0 - t1);
   UInt32 diff = diff_fs.getInternalDataForced() / 1000000; // Convert native FS to NS
   UInt32 bin = floorLog2(diff);
   if (bin >= SKEW_BINS) bin = SKEW_BINS - 1;
   skew_hist[core_id][bin]++;
   skew_sum[core_id] += diff;
   skew_count[core_id]++;
}

void skew_record_all() {
   if (!skew_numcores)
      skew_numcores = Sim()->getConfig()->getApplicationCores();
   for(unsigned int i = 1; i < skew_numcores; ++i)
      if (Sim()->getThreadManager()->isThreadRunning(i))
         skew_record(i);
}

void skew_report(INT32 code, VOID *v) {
   printf("-- Skew --\n");
   printf("CPU  ");
   UInt64 total_hist[SKEW_BINS] = { 0 }, total_sum = 0, total_count = 0;
   for(int j = 0; j < SKEW_BINS; ++j)
      if (j < 10)
         printf("%4u   ", 1 << j);
      else
         printf("%4uk  ", 1 << (j - 10));
   printf("\n");
   for(unsigned int i = 1; i < skew_numcores; ++i) {
      printf("%3u: ", i);
      for(int j = 0; j < SKEW_BINS; ++j) {
         printf("%4.1f%%  ", 100. * skew_hist[i][j] / skew_count[i]);
         total_hist[j] += skew_hist[i][j];
      }
      printf("\n");
      total_sum += skew_sum[i];
      total_count += skew_count[i];
   }
   SInt64 pct90, pct90_limit = .9 * total_count;
   for(pct90 = 0; pct90_limit > 0 && pct90 < SKEW_BINS; ++pct90)
      pct90_limit -= total_hist[pct90];
   printf("Avg = %.1f, 90%% = %u\n", total_sum / (float)total_count, 1 << (pct90 - 1));
}

bool skew_report_enable() {
   return Sim()->getConfig()->getEnableSyncReport();
}


static bool enabled()
{
   return Sim()->getConfig()->getEnableSync();
}

static bool abortFunc(THREADID threadIndex)
{
   if (PIN_IsActionPending(threadIndex))
      return true;
   else
      return false;
}

void handlePeriodicSync(THREADID threadIndex)
{
   Thread* thread = localStore[threadIndex].thread;
   Core* core = thread->getCore();
   assert(core);

   ClockSkewMinimizationClient *client = thread->getClockSkewMinimizationClient();
   if (client)
   {
      #ifdef ENABLE_PERF_MODEL_OWN_THREAD
         thread->getPerformanceModel()->setHold(true);
      #endif
      client->synchronize(
         SubsecondTime::Zero(), false,
         appDebug_enabled ? (bool (*)(void*))abortFunc : NULL,
         (void*)(unsigned long)threadIndex
      );
      #ifdef ENABLE_PERF_MODEL_OWN_THREAD
         core->getPerformanceModel()->setHold(false);
      #endif
   }

   if (!Sim()->isRunning())
   {
      // Main thread has exited, but we still seem to be running.
      // Don't touch any more simulator structure as they're being deallocated right now.
      // Just wait here until the whole application terminates us.
      while(1)
         PIN_Yield();
   }

   if (skew_do && core->getId() == 0)
      skew_record_all();
}

void addPeriodicSync(TRACE trace, INS ins, InstMode::inst_mode_t inst_mode)
{
   if (enabled() || skew_report_enable()) {
      // There is no concept of time except when in DETAILED mode (the core performance model keeps this)
      // so for other modes, avoid the overhead of this call
      INSTRUMENT(INSTR_IF_DETAILED(inst_mode), trace, ins, IPOINT_BEFORE, AFUNPTR(handlePeriodicSync), IARG_THREAD_ID, IARG_END);
   }

   if (! skew_report_added) {
      skew_do = skew_report_enable();
      if (skew_do)
         PIN_AddFiniFunction(skew_report, 0);
      skew_report_added = true;
   }

   // TODO: Early-exit out of sync isn't very reliable: when the MCP does something that triggers a Pin action,
   // such as changing instrumentation mode, the threads may exit as a consequense of this but before they
   // get their MCP message, and potentially before the MCP was done doing whatever setup we should have
   // waited for. Thus, only use early-exit when appdebug is actually in use.
   appDebug_enabled = PIN_GetDebugStatus() != DEBUG_STATUS_DISABLED;
}
