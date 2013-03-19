#include "interval_performance_model.h"
#include "config.hpp"

#include <cstdio>

IntervalPerformanceModel::IntervalPerformanceModel(Core *core, int misprediction_penalty)
    : MicroOpPerformanceModel(core, !Sim()->getCfg()->getBoolArray("perf_model/core/interval_timer/issue_memops_at_dispatch", core->getId()))
    , interval_timer(core,
       this,
       m_core_model,
       misprediction_penalty,
       Sim()->getCfg()->getIntArray("perf_model/core/interval_timer/dispatch_width", core->getId()),
       Sim()->getCfg()->getIntArray("perf_model/core/interval_timer/window_size", core->getId()),
       Sim()->getCfg()->getBoolArray("perf_model/core/interval_timer/issue_contention", core->getId())
      )
{
}

IntervalPerformanceModel::~IntervalPerformanceModel()
{
   interval_timer.free(); // Free Windows, and remaining DynamicMicroOps, before deleting the allocator
}

boost::tuple<uint64_t,uint64_t> IntervalPerformanceModel::simulate(const std::vector<DynamicMicroOp*>& insts)
{
   return interval_timer.simulate(insts);
}

void IntervalPerformanceModel::notifyElapsedTimeUpdate()
{
   interval_timer.synchronize(m_elapsed_time.getCycleCount());
}
