#ifndef PERFORMANCE_MODEL_H
#define PERFORMANCE_MODEL_H
// This class represents the actual performance model for a given core

#include "instruction.h"
#include "basic_block.h"
#include "fixed_types.h"
#include "mt_circular_queue.h"
#include "lock.h"
#include "dynamic_instruction_info.h"
#include "subsecond_time.h"

#include <queue>
#include <iostream>

// Forward Decls
class Core;
class BranchPredictor;

class PerformanceModel
{
public:
   static const SubsecondTime DyninsninfoNotAvailable() { return SubsecondTime::MaxTime(); }

   PerformanceModel(Core* core);
   virtual ~PerformanceModel();

   void queueDynamicInstruction(Instruction *i);
   void queueBasicBlock(BasicBlock *basic_block);
   void iterate();

   virtual void outputSummary(std::ostream &os) const = 0;

   virtual SubsecondTime getElapsedTime() const = 0;
   virtual void resetElapsedTime() = 0;
   virtual UInt64 getInstructionCount() const = 0;
   virtual SubsecondTime getNonIdleElapsedTime() const = 0;

   void countInstructions(IntPtr address, UInt32 count);
   void pushDynamicInstructionInfo(DynamicInstructionInfo &i);
   void popDynamicInstructionInfo();
   DynamicInstructionInfo* getDynamicInstructionInfo(const Instruction &instruction);

   static PerformanceModel *create(Core* core);

   BranchPredictor *getBranchPredictor() { return m_bp; }
   BranchPredictor const* getConstBranchPredictor() const { return m_bp; }

   void disable();
   void enable();
   bool isEnabled() { return m_enabled; }
   void setHold(bool hold) { m_hold = hold; }

protected:
   friend class SpawnInstruction;

   virtual void setElapsedTime(SubsecondTime time) = 0;
   virtual void incrementElapsedTime(SubsecondTime time) = 0;

   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      typedef MTCircularQueue<DynamicInstructionInfo> DynamicInstructionInfoQueue;
      typedef MTCircularQueue<BasicBlock *> BasicBlockQueue;
   #else
      typedef CircularQueue<DynamicInstructionInfo> DynamicInstructionInfoQueue;
      typedef CircularQueue<BasicBlock *> BasicBlockQueue;
   #endif

   Core* getCore() { return m_core; }

private:

   DynamicInstructionInfo* getDynamicInstructionInfo();

   virtual bool handleInstruction(Instruction const* instruction) = 0;

   Core* m_core;

   bool m_enabled;

   bool m_hold;

   BasicBlockQueue m_basic_block_queue;
   DynamicInstructionInfoQueue m_dynamic_info_queue;

   UInt32 m_current_ins_index;

   BranchPredictor *m_bp;
};

#endif
