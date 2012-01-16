#ifndef __MEMORY_DEPENDENCIES_H
#define __MEMORY_DEPENDENCIES_H

#include "fixed_types.h"
#include "micro_op.h"

class MemoryDependencies {
private:
  // Hash map with key = address, value = sequence number of last writing instruction
  std::unordered_map<uint64_t, uint64_t>* producers;

  uint64_t membar;
public:
  MemoryDependencies();

  ~MemoryDependencies();

  void setDependencies(MicroOp& microOp, uint64_t lowestValidSequenceNumber);

  void clear();
};

#endif /* __MEMORY_DEPENDENCIES_H */
