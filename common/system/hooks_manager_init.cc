#include "hooks_manager.h"

#include "hooks_py.h"

#include "subsecond_time.h"
#include "fixed_point.h"
#include "simulator.h"
#include "stats.h"
#include "dvfs_manager.h"


// Example live-analysis code: print out the IPC for core 0

void hook_print_core0_ipc(void*, subsecond_time_t _time)
{
   SubsecondTime time(_time);
   static StatsMetricBase *s_time = NULL;
   static UInt64 l_time = 0;
   static StatsMetricBase *s_instructions = NULL;
   static UInt64 l_instructions = 0;
   static const ComponentPeriod *clock = NULL;

   if (!s_time) {
      s_time = Sim()->getStatsManager()->getMetricObject("performance_model", 0, "elapsed_time");
      s_instructions = Sim()->getStatsManager()->getMetricObject("performance_model", 0, "instruction_count");
      clock = Sim()->getDvfsManager()->getCoreDomain(0);
      LOG_ASSERT_ERROR(s_time && s_instructions && clock, "Could not find stats / dvfs domain for core 0");
   } else {
      UInt64 d_instructions = s_instructions->recordMetric() - l_instructions;
      UInt64 d_time = s_time->recordMetric() - l_time;
      UInt64 d_cycles = SubsecondTime::divideRounded(SubsecondTime::FS(d_time), *clock);
      if (d_cycles) {
         FixedPoint ipc = FixedPoint(d_instructions) / d_cycles;
         printf("t = %" PRIu64 " ns, ipKc = %" PRId64 "\n", time.getNS(), FixedPoint::floor(ipc * 1000));
      }
   }

   l_time = s_time->recordMetric();
   l_instructions = s_instructions->recordMetric();
}


// Handy place to instantiate all classes that need to register hooks but are otherwise unconnected to the basic simulator
void HooksManager::init(void)
{
   HooksPy::init();
   //registerHook(HookType::HOOK_PERIODIC, (HookCallbackFunc)hook_print_core0_ipc, NULL);
}

void HooksManager::fini(void)
{
   HooksPy::fini();
}
