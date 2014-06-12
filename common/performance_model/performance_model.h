#ifndef PERFORMANCE_MODEL_H
#define PERFORMANCE_MODEL_H
// This class represents the actual performance model for a given core

#include "fixed_types.h"
#include "mt_circular_queue.h"
#include "lock.h"
#include "subsecond_time.h"
#include "instruction_tracer.h"
#include "hit_where.h"

#include <queue>
#include <iostream>

// Forward Decls
class Core;
class BranchPredictor;
class FastforwardPerformanceModel;
class Instruction;
class PseudoInstruction;
class DynamicInstruction;
class Allocator;

class PerformanceModel
{
public:
   PerformanceModel(Core* core);
   virtual ~PerformanceModel();

   void queueInstruction(DynamicInstruction *i);
   void queuePseudoInstruction(PseudoInstruction *i);
   void handleIdleInstruction(PseudoInstruction *i);
   void iterate();
   virtual void synchronize();

   UInt64 getInstructionCount() const { return m_instruction_count; }

   SubsecondTime getElapsedTime() const { return m_elapsed_time.getElapsedTime(); }
   SubsecondTime getNonIdleElapsedTime() const { return getElapsedTime() - m_idle_elapsed_time.getElapsedTime(); }

   void countInstructions(IntPtr address, UInt32 count);
   void handleMemoryLatency(SubsecondTime latency, HitWhere::where_t hit_where);
   void handleBranchMispredict();

   static PerformanceModel *create(Core* core);

   BranchPredictor *getBranchPredictor() { return m_bp; }
   BranchPredictor const* getConstBranchPredictor() const { return m_bp; }

   FastforwardPerformanceModel *getFastforwardPerformanceModel() { return m_fastforward_model; }
   FastforwardPerformanceModel const* getFastforwardPerformanceModel() const { return m_fastforward_model; }

   DynamicInstruction* createDynamicInstruction(Instruction *ins, IntPtr eip);

   void traceInstruction(const DynamicMicroOp *uop, InstructionTracer::uop_times_t *times)
   {
      if (m_instruction_tracer)
         m_instruction_tracer->traceInstruction(uop, times);
   }

   virtual void barrierEnter() { }
   virtual void barrierExit() { }

   void disable();
   void enable();
   bool isEnabled() { return m_enabled; }
   void setHold(bool hold) { m_hold = hold; }

   bool isFastForward() { return m_fastforward; }
   void setFastForward(bool fastforward, bool detailed_sync = true)
   {
      if (m_fastforward == fastforward)
         return;
      m_fastforward = fastforward;
      m_detailed_sync = detailed_sync;
      // Fastforward performance model has controlled time for a while, now let the detailed model know time has advanced
      if (fastforward == false)
      {
         enableDetailedModel();
         notifyElapsedTimeUpdate();
      }
      else
         disableDetailedModel();
   }

protected:
   friend class SpawnInstruction;
   friend class FastforwardPerformanceModel;

   void setElapsedTime(SubsecondTime time);
   void incrementElapsedTime(SubsecondTime time) { m_elapsed_time.addLatency(time); }
   void incrementIdleElapsedTime(SubsecondTime time);

   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      typedef MTCircularQueue<DynamicInstruction*> InstructionQueue;
   #else
      typedef CircularQueue<DynamicInstruction*> InstructionQueue;
   #endif

   Core* getCore() { return m_core; }

private:

   // Simulate a single instruction
   virtual void handleInstruction(DynamicInstruction *instruction) = 0;

   // When time is jumped ahead outside of control of the performance model (synchronization instructions, etc.)
   // notify it here. This may be used to synchronize internal time or to flush various instruction queues
   virtual void notifyElapsedTimeUpdate() {}
   // Called when the detailed model is enabled/disabled. Used to release threads from the SMT barrier.
   virtual void enableDetailedModel() {}
   virtual void disableDetailedModel() {}

   Core* m_core;
   Allocator *m_dynins_alloc;

   bool m_enabled;

   bool m_fastforward;
   FastforwardPerformanceModel* m_fastforward_model;
   bool m_detailed_sync;

   bool m_hold;

protected:
   UInt64 m_instruction_count;

   ComponentTime m_elapsed_time;
private:
   ComponentTime m_idle_elapsed_time;

   SubsecondTime m_cpiStartTime;
   // CPI components for Sync and Recv instructions
   SubsecondTime m_cpiSyncFutex;
   SubsecondTime m_cpiSyncPthreadMutex;
   SubsecondTime m_cpiSyncPthreadCond;
   SubsecondTime m_cpiSyncPthreadBarrier;
   SubsecondTime m_cpiSyncJoin;
   SubsecondTime m_cpiSyncPause;
   SubsecondTime m_cpiSyncSleep;
   SubsecondTime m_cpiSyncSyscall;
   SubsecondTime m_cpiSyncUnscheduled;
   SubsecondTime m_cpiSyncDvfsTransition;
   SubsecondTime m_cpiRecv;

   InstructionQueue m_instruction_queue;

   UInt32 m_current_ins_index;

   BranchPredictor *m_bp;

   InstructionTracer *m_instruction_tracer;
};

#endif
