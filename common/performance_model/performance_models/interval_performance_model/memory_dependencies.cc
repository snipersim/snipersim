#include "memory_dependencies.h"

MemoryDependencies::MemoryDependencies()
   : producers(new std::unordered_map<uint64_t, uint64_t>(81920))
{
   clear();
}

MemoryDependencies::~MemoryDependencies()
{
   delete producers;
}

void MemoryDependencies::setDependencies(MicroOp& microOp, uint64_t lowestValidSequenceNumber)
{
   if (microOp.isLoad())
   {
      uint64_t physicalAddress = microOp.getLoadAccess().phys;
      std::unordered_map<uint64_t, uint64_t>::iterator it = producers->find(physicalAddress);
      if (it != producers->end()) /* producer found */
      {
         uint64_t producerSequenceNumber = it->second;
         if (producerSequenceNumber >= lowestValidSequenceNumber)
         {
            microOp.addDependency(producerSequenceNumber);
         }
         else
         {
            producers->erase(physicalAddress);
         }
      }

      // Purposely using & instead of && to avoid branch instructions
      // at the cost of an extra comparison
      if ((membar != INVALID_SEQNR) & (membar > lowestValidSequenceNumber))
      {
         microOp.addDependency(membar);
      }

   }
   else if (microOp.isStore())
   {
      uint64_t physicalAddress = microOp.getStoreAccess().phys;
      (*producers)[physicalAddress] = microOp.getSequenceNumber();

      // Stores are also dependent on membars
      // Purposely using & instead of && to avoid branch instructions
      // at the cost of an extra comparison
      if ((membar != INVALID_SEQNR) & (membar > lowestValidSequenceNumber))
      {
         microOp.addDependency(membar);
      }

   }
   else if (microOp.isMemBarrier())
   {
      // And membars are dependent on previous membars
      // Purposely using & instead of && to avoid branch instructions
      // at the cost of an extra comparison
      if ((membar != INVALID_SEQNR) & (membar > lowestValidSequenceNumber))
      {
         microOp.addDependency(membar);
      }

      // Actual MFENCE instruction
      // All new instructions will have higher sequence numbers
      // Therefore, this will remain sorted
      membar = microOp.getSequenceNumber();
   }
}

void MemoryDependencies::clear()
{
   producers->clear();
   membar = INVALID_SEQNR;
}
