#ifndef WINDOWS_HPP_
#define WINDOWS_HPP_

#include "micro_op.h"
#include "register_dependencies.h"
#include "memory_dependencies.h"

#define INVALID_SEQNR 0xffffffffffffffff

enum WindowStopDispatchReason {
   WIN_STOP_DISPATCH_NO_REASON = 0,
   WIN_STOP_DISPATCH_FP_ADDSUB = 1,
   WIN_STOP_DISPATCH_FP_MULDIV = 2,
   WIN_STOP_DISPATCH_LOAD = 4,
   WIN_STOP_DISPATCH_STORE = 8,
   WIN_STOP_DISPATCH_GENERIC = 16,
   WIN_STOP_DISPATCH_BRANCH = 32,
   WIN_STOP_DISPATCH_SIZE = 64,
};

String WindowStopDispatchReasonStringHelper(WindowStopDispatchReason r);
String WindowStopDispatchReasonString(WindowStopDispatchReason r);

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

CpContrType getCpContrType(const MicroOp& uop);
String CpContrTypeString(CpContrType r);

class Windows {
public:
  int windowIndex(int index) const;
  int incrementIndex(int index) const;
  int decrementIndex(int index) const;

  Windows(int windowSize, bool doFunctionalUnitContention);

  ~Windows();

  void clear();

  bool wIsFull() const;

  bool wIsEmpty() const;

  /**
   * Add the microOperation to the window and calculate its dependencies.
   */
  void add(const MicroOp& microOp);

  MicroOp& getInstruction(uint64_t sequenceNumber) const;

  MicroOp& getLastAdded() const;

  MicroOp& getInstructionToDispatch() const;

  MicroOp& getOldestInstruction() const;

  WindowStopDispatchReason dispatchInstruction();

  void clearOldWindow(uint64_t newCpHead);

  bool windowContains(uint64_t sequenceNumber) const;

  bool oldWindowContains(uint64_t sequenceNumber) const;

  int getOldWindowLength() const;

  uint64_t getCriticalPathHead() const;

  uint64_t getCriticalPathTail() const;

  int getCriticalPathLength() const;

  uint64_t longLatencyOperationLatency(MicroOp& uop);

  uint64_t updateCriticalPathTail(MicroOp& uop);

  int getMinimalFlushLatency(int width) const;

  uint64_t getCpContrFraction(CpContrType type) const;

  class Iterator {
    private:
      int index;
      const int stop;
      const Windows* const windows;
    public:
      Iterator(const Windows* const windows, int start, int stop);
      bool hasNext();
      MicroOp& next();
  };

  Iterator getWindowIterator() const;
  Iterator getOldWindowIterator() const;

  int calculateBranchResolutionLatency();
  uint64_t getEffectiveCriticalPathLength(uint64_t critical_path_length);

  String toString();

private:
  int m_window_size;
  int m_double_window_size;

  MicroOp* const m_double_window;
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

  uint64_t m_count_byport[MicroOp::UOP_PORT_SIZE];
  uint64_t m_cpcontr_bytype[CPCONTR_TYPE_SIZE];
  uint64_t m_cpcontr_total;

  MicroOp& getInstructionByIndex(int index) const;

  WindowStopDispatchReason shouldStopDispatch();

  void addFunctionalUnitStats(const MicroOp &uop);
  void removeFunctionalUnitStats(const MicroOp &uop);
  void clearFunctionalUnitStats();

  friend class RegisterDependencies;
  friend class MemoryDependencies;
};

#endif /* WINDOWS_HPP_ */
