#ifndef MAGIC_PERFORMANCE_MODEL_H
#define MAGIC_PERFORMANCE_MODEL_H

#include "performance_model.h"
#include "subsecond_time.h"

class MagicPerformanceModel : public PerformanceModel
{
public:
   MagicPerformanceModel(Core *core);
   ~MagicPerformanceModel();

   void outputSummary(std::ostream &os) const;

   UInt64 getInstructionCount() const { return m_instruction_count; }
   SubsecondTime getElapsedTime() const { return m_elapsed_time.getElapsedTime(); }
   void resetElapsedTime() { m_elapsed_time.reset(); }
   SubsecondTime getNonIdleElapsedTime() const { return m_elapsed_time.getElapsedTime(); /* TODO: subtract idle time */ }

protected:
   void setElapsedTime(SubsecondTime time);
   void incrementElapsedTime(SubsecondTime time);

private:
   bool handleInstruction(Instruction const* instruction);

   bool isModeled(Instruction const* instruction) const;

   UInt64 m_instruction_count;
   ComponentTime m_elapsed_time;
};

#endif
