#ifndef __MEMORY_DEPENDENCIES_H
#define __MEMORY_DEPENDENCIES_H

#include "fixed_types.h"
#include "circular_queue.h"
#include "dynamic_micro_op.h"

class MemoryDependencies
{
   private:
      struct Producer
      {
         uint64_t seqnr;
         uint64_t address;
      };
      // List of all active writers, ordered by sequence number
      // This makes it easy to remove old entries, just compare the front of the queue with lowestValidSequenceNumber
      // Finding the latest producer is a linear search, starting from the tail (there may be multiple entries
      // with the same address, we want the latest one).
      // Maximum number of entries is the number of instructions in the ROB, actual number of entries equals
      // the number of writes in the ROB which is usually not larger than around 20.
      // More fancy data structures, such as an underdered_map keyed on address, have a much higher overhead on insertion
      // and put significant pressure on malloc()/free(), and are therefore not recommended.
      CircularQueue<Producer> producers;
      uint64_t membar;

      void add(uint64_t sequenceNumber, uint64_t address);
      uint64_t find(uint64_t address);
      void clean(uint64_t lowestValidSequenceNumber);

   public:
      MemoryDependencies();
      ~MemoryDependencies();

      void setDependencies(DynamicMicroOp &microOp, uint64_t lowestValidSequenceNumber);
      void clear();
};

#endif /* __MEMORY_DEPENDENCIES_H */
