#ifndef ROB_SMT_PERFORMANCE_MODEL_H
#define ROB_SMT_PERFORMANCE_MODEL_H

#include "micro_op_performance_model.h"
#include "rob_smt_timer.h"

#include <unordered_map>

class RobSmtPerformanceModel : public MicroOpPerformanceModel
{
public:
   RobSmtPerformanceModel(Core *core);
   ~RobSmtPerformanceModel();

protected:
   virtual boost::tuple<uint64_t,uint64_t> simulate(const std::vector<DynamicMicroOp*>& insts);
   virtual void notifyElapsedTimeUpdate();
   virtual void enableDetailedModel();
   virtual void disableDetailedModel();
   virtual void synchronize();
private:
   RobSmtTimer *m_rob_timer;
   UInt8 m_thread_id;
   bool m_enabled;

   static std::unordered_map<core_id_t, RobSmtTimer*> s_rob_timers;
   static RobSmtTimer* getRobTimer(Core *core, RobSmtPerformanceModel *perf, const CoreModel *core_model);
};

#endif
