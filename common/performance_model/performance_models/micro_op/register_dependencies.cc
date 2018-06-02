#include "register_dependencies.h"
#include "dynamic_micro_op.h"

RegisterDependencies::RegisterDependencies()
{
   clear();
}

void RegisterDependencies::setDependencies(DynamicMicroOp& microOp, uint64_t lowestValidSequenceNumber)
{
   // Create the dependencies for the microOp
   for(uint32_t i = 0; i < microOp.getMicroOp()->getSourceRegistersLength(); i++)
   {
      dl::Decoder::decoder_reg sourceRegister = microOp.getMicroOp()->getSourceRegister(i);
      uint64_t producerSequenceNumber;
      LOG_ASSERT_ERROR(sourceRegister < Sim()->getDecoder()->last_reg(), "Source register src[%u]=%u is invalid", i, sourceRegister);
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
   for(uint32_t i = 0; i < microOp.getMicroOp()->getDestinationRegistersLength(); i++)
   {
      uint32_t destinationRegister = microOp.getMicroOp()->getDestinationRegister(i);
      LOG_ASSERT_ERROR(destinationRegister < Sim()->getDecoder()->last_reg(), "Destination register dst[%u] = %u is invalid", i, destinationRegister);
      producers[destinationRegister] = microOp.getSequenceNumber();
   }

}

uint64_t RegisterDependencies::peekProducer(dl::Decoder::decoder_reg reg, uint64_t lowestValidSequenceNumber)
{
   if (reg == dl::Decoder::DL_REG_INVALID)
      return INVALID_SEQNR;

   uint64_t producerSequenceNumber = producers[reg];
   if (producerSequenceNumber == INVALID_SEQNR || producerSequenceNumber < lowestValidSequenceNumber)
      return INVALID_SEQNR;

   return producerSequenceNumber;
}

void RegisterDependencies::clear()
{
   for(uint32_t i = 0; i < Sim()->getDecoder()->last_reg(); i++)
   {
      producers[i] = INVALID_SEQNR;
   }
}
