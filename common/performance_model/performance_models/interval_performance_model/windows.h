/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#ifndef WINDOWS_HPP_
#define WINDOWS_HPP_

#include "micro_op.h"
#include "register_dependencies.h"
#include "memory_dependencies.h"

class Core;
class IntervalContention;

enum CpContrType {
   CPCONTR_TYPE_FP_ADDSUB,
   CPCONTR_TYPE_FP_MULDIV,
   CPCONTR_TYPE_LOAD_L1,
   CPCONTR_TYPE_LOAD_L2,
   CPCONTR_TYPE_LOAD_L3,
   CPCONTR_TYPE_LOAD_LX,
   CPCONTR_TYPE_STORE,
   CPCONTR_TYPE_GENERIC,
   CPCONTR_TYPE_BRANCH,
   CPCONTR_TYPE_SIZE,
};

class Windows {
public:

   struct WindowEntry
   {
      void initialize(DynamicMicroOp* micro_op);

      /** The index of the microOperation in the window. Constant! */
      uint32_t windowIndex;


      /** We own this, and have to delete it */
      DynamicMicroOp *uop;

      /** The cycle in which the instruction is executed. */
      uint64_t execTime;
      /** The cycle in which the instruction is dispatched. */
      uint64_t dispatchTime;
      /** The cycle in which the instruction was fetched. This clock is not synchronized with the simulator clock. */
      uint64_t fetchTime;
      /** The number of cycles this instruction contributes to the critical path. */
      uint64_t cpContr;

      enum {ICACHE_OVERLAP = 1, BPRED_OVERLAP = 2, DCACHE_OVERLAP = 4};
      /** The latency of the microInstruction can be overlapped by a long latency load. The flag states what is overlapped: a icache miss, a branch mispredict or a dcache miss. */
      uint32_t overlapFlags;

      enum {NO_DEP = 0, DATA_DEP = 1, INDEP_MISS = 2};
      /** Used during the block window algorithm, shouldn't be here -> has to be int[doubleWindowSize] in IntervalTimer or Windows. */
      uint32_t dependent;

      uint64_t cphead;
      uint64_t cptail;
      uint64_t maxProducer;

      uint32_t getWindowIndex() const { return this->windowIndex; }
      void setWindowIndex(uint32_t index) { this->windowIndex = index; }

      DynamicMicroOp* getDynMicroOp() { return this->uop; }
      const DynamicMicroOp* getDynMicroOp() const { return this->uop; }
      /* Some proxy functions */
      const MicroOp* getMicroOp() const { return this->getDynMicroOp()->getMicroOp(); }
      uint64_t getSequenceNumber() const { return this->getDynMicroOp()->getSequenceNumber(); }

      uint64_t getExecTime() const { return this->execTime; }
      void setExecTime(uint64_t time) { this->execTime = time; }

      uint64_t getDispatchTime() const { return this->dispatchTime; }
      void setDispatchTime(uint64_t time) { this->dispatchTime = time; }

      uint64_t getFetchTime() const { return this->fetchTime; }
      void setFetchTime(uint64_t time) { this->fetchTime = time; }

      uint64_t getCpContr() const { return this->cpContr; }
      void setCpContr(uint64_t cpContr) { this->cpContr = cpContr; }

      void addOverlapFlag(uint32_t flag) { this->overlapFlags |= flag; }
      bool hasOverlapFlag(uint32_t flag) const { return this->overlapFlags & flag; }

      void clearDependent() { this->dependent = NO_DEP; }
      bool isDependent() const { return this->dependent == DATA_DEP; }
      bool isIndependent() const { return this->dependent == INDEP_MISS; }
      void setDataDependent() { this->dependent = DATA_DEP; }
      void setIndependentMiss() { this->dependent = INDEP_MISS; }
   };

  int windowIndex(int index) const;
  int incrementIndex(int index) const;
  int decrementIndex(int index) const;

  Windows(int windowSize, bool doFunctionalUnitContention, Core *core, const CoreModel *core_model);

  ~Windows();

  void clear();

  bool wIsFull() const;

  bool wIsEmpty() const;

  /**
   * Add the microOperation to the window and calculate its dependencies.
   */
  void add(DynamicMicroOp* microOp);

  WindowEntry& getInstruction(uint64_t sequenceNumber) const;

  WindowEntry& getLastAdded() const;

  WindowEntry& getInstructionToDispatch() const;

  WindowEntry& getOldestInstruction() const;

  void dispatchInstruction();

  void clearOldWindow(uint64_t newCpHead);

  bool windowContains(uint64_t sequenceNumber) const;

  bool oldWindowContains(uint64_t sequenceNumber) const;

  int getOldWindowLength() const;

  uint64_t getCriticalPathHead() const;

  uint64_t getCriticalPathTail() const;

  int getCriticalPathLength() const;

  uint64_t longLatencyOperationLatency(WindowEntry& uop);

  uint64_t updateCriticalPathTail(WindowEntry& uop);

  int getMinimalFlushLatency(int width) const;

  uint64_t getCpContrFraction(CpContrType type, uint64_t effective_cp_length) const;

  class Iterator {
    private:
      int index;
      const int stop;
      const Windows* const windows;
    public:
      Iterator(const Windows* const windows, int start, int stop);
      bool hasNext();
      WindowEntry& next();
  };

  Iterator getWindowIterator() const;
  Iterator getOldWindowIterator() const;

  int calculateBranchResolutionLatency();
  uint64_t getEffectiveCriticalPathLength(uint64_t critical_path_length, bool update_reason);

  String toString();

private:
  const CoreModel *m_core_model;
  IntervalContention *m_interval_contention;

  int m_window_size;
  int m_double_window_size;

  WindowEntry* const m_double_window;
  uint32_t* const m_exec_time_map; // Used to store the execution time of the producers when calculating the branch resolution time.

  bool m_do_functional_unit_contention;

  uint64_t m_next_sequence_number;

  RegisterDependencies* const m_register_dependencies;
  MemoryDependencies* const m_memory_dependencies;

  int m_window_head__old_window_tail;
  int m_window_tail;
  int m_old_window_head;

  int m_window_length;
  int m_old_window_length;

  uint64_t m_critical_path_head;
  uint64_t m_critical_path_tail;

  uint64_t m_cpcontr_bytype[CPCONTR_TYPE_SIZE];
  uint64_t m_cpcontr_total;

  WindowEntry& getInstructionByIndex(int index) const;

  void addFunctionalUnitStats(const WindowEntry &uop);
  void removeFunctionalUnitStats(const WindowEntry &uop);
  void clearFunctionalUnitStats();

  friend class RegisterDependencies;
  friend class MemoryDependencies;
};

CpContrType getCpContrType(const Windows::WindowEntry& uop);
String CpContrTypeString(CpContrType r);

#endif /* WINDOWS_HPP_ */
