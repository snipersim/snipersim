#include "memory_dependencies.h"

MemoryDependencies::MemoryDependencies()
   : producers(1024) // Maximum size should be one ROB worth of instructions
{
   clear();
}

MemoryDependencies::~MemoryDependencies()
{
}

void MemoryDependencies::setDependencies(DynamicMicroOp &microOp, uint64_t lowestValidSequenceNumber)
{
   // Remove all entries that are now below lowestValidSequenceNumber
   clean(lowestValidSequenceNumber);

   if (microOp.getMicroOp()->isLoad())
   {
      uint64_t physicalAddress = microOp.getLoadAccess().phys;
      uint64_t producerSequenceNumber = find(physicalAddress);
      if (producerSequenceNumber != INVALID_SEQNR) /* producer found */
      {
         microOp.addDependency(producerSequenceNumber);
      }

      if ((membar != INVALID_SEQNR) && (membar > lowestValidSequenceNumber))
      {
         microOp.addDependency(membar);
      }
   }
   else if (microOp.getMicroOp()->isStore())
   {
      uint64_t physicalAddress = microOp.getStoreAccess().phys;
      add(microOp.getSequenceNumber(), physicalAddress);

      // Stores are also dependent on membars
      if ((membar != INVALID_SEQNR) && (membar > lowestValidSequenceNumber))
      {
         microOp.addDependency(membar);
      }

   }
   else if (microOp.getMicroOp()->isMemBarrier())
   {
      // And membars are dependent on previous membars
      if ((membar != INVALID_SEQNR) && (membar > lowestValidSequenceNumber))
      {
         microOp.addDependency(membar);
      }

      // Actual MFENCE instruction
      // All new instructions will have higher sequence numbers
      // Therefore, this will remain sorted
      membar = microOp.getSequenceNumber();
   }
}

void MemoryDependencies::add(uint64_t sequenceNumber, uint64_t address)
{
   Producer producer = {sequenceNumber, address};
   producers.push(producer);
}

uint64_t MemoryDependencies::find(uint64_t address)
{
   // There may be multiple entries with the same address, we want the latest one so traverse list in reverse order
   for(int i = producers.size() - 1; i >= 0; --i)
      if (producers.at(i).address == address)
         return producers.at(i).seqnr;
   return INVALID_SEQNR;
}

void MemoryDependencies::clean(uint64_t lowestValidSequenceNumber)
{
   while(!producers.empty() && producers.front().seqnr < lowestValidSequenceNumber)
      producers.pop();
}

void MemoryDependencies::clear()
{
   while(!producers.empty())
      producers.pop();
   membar = INVALID_SEQNR;
}
