#include "register_dependencies.h"

RegisterDependencies::RegisterDependencies()
{
   clear();
}

void RegisterDependencies::setDependencies(MicroOp& microOp, uint64_t lowestValidSequenceNumber)
{
   // Create the dependencies for the microOp
   for(uint32_t i = 0; i < microOp.getSourceRegistersLength(); i++)
   {
      uint32_t sourceRegister = microOp.getSourceRegister(i);
      uint64_t producerSequenceNumber;
      LOG_ASSERT_ERROR(sourceRegister < TOTAL_NUM_REGISTERS, "Source register src[%u]=%u is invalid", i, sourceRegister);
      if ((producerSequenceNumber = producers[sourceRegister]) != INVALID_SEQNR)
      {
         if (producerSequenceNumber >= lowestValidSequenceNumber)
         {
            microOp.addDependency(producerSequenceNumber);
         }
         else
         {
            producers[sourceRegister] = INVALID_SEQNR;
         }
      }
   }

   // Update the producers
   for(uint32_t i = 0; i < microOp.getDestinationRegistersLength(); i++)
   {
      uint32_t destinationRegister = microOp.getDestinationRegister(i);
      LOG_ASSERT_ERROR(destinationRegister < TOTAL_NUM_REGISTERS, "Destination register dst[%u] = %u is invalid", i, destinationRegister);
      producers[destinationRegister] = microOp.getSequenceNumber();
   }

}

void RegisterDependencies::clear()
{
   for(uint32_t i = 0; i < TOTAL_NUM_REGISTERS; i++)
   {
      producers[i] = INVALID_SEQNR;
   }
}
