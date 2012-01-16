#ifndef ONEIPC_PERFORMANCE_MODEL_H
#define ONEIPC_PERFORMANCE_MODEL_H

#include "performance_model.h"

class OneIPCPerformanceModel : public PerformanceModel
{
public:
   OneIPCPerformanceModel(Core *core);
   ~OneIPCPerformanceModel();

   void outputSummary(std::ostream &os) const;

   UInt64 getInstructionCount() const { return m_instruction_count; }
   SubsecondTime getElapsedTime() const { return m_elapsed_time.getElapsedTime(); }
   void resetElapsedTime() { m_elapsed_time.reset(); }
   SubsecondTime getNonIdleElapsedTime() const { return m_elapsed_time.getElapsedTime() - m_idle_elapsed_time.getElapsedTime(); }

protected:
   void setElapsedTime(SubsecondTime time);
   void incrementElapsedTime(SubsecondTime time);

private:
   bool handleInstruction(Instruction const* instruction);

   bool isModeled(Instruction const* instruction) const;

   UInt64 m_latency_cutoff;

   UInt64 m_instruction_count;
   ComponentTime m_elapsed_time;
   ComponentTime m_idle_elapsed_time;
};

#endif // ONEIPC_PERFORMANCE_MODEL_H
