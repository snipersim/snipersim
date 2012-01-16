#ifndef SIMPLE_PERFORMANCE_MODEL_H
#define SIMPLE_PERFORMANCE_MODEL_H

#include "performance_model.h"

class SimplePerformanceModel : public PerformanceModel
{
public:
   SimplePerformanceModel(Core *core);
   ~SimplePerformanceModel();

   void outputSummary(std::ostream &os) const;

   UInt64 getInstructionCount() const { return m_instruction_count; }
   SubsecondTime getElapsedTime() const { return m_elapsed_time.getElapsedTime(); }
   void resetElapsedTime() { m_elapsed_time.reset(); }
   void incrementElapsedTime(SubsecondTime time);
   SubsecondTime getNonIdleElapsedTime() const { return m_elapsed_time.getElapsedTime(); /* TODO: subtract idle time */ }

protected:
   void setElapsedTime(SubsecondTime time);

private:
   bool handleInstruction(Instruction const* instruction);

   UInt64 m_instruction_count;
   ComponentTime m_elapsed_time;
};

#endif
