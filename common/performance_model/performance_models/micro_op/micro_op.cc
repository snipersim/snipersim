#include "micro_op.h"
#include "instruction.h"

//extern "C" {
//#include <xed-decoded-inst.h>
//}

#include <assert.h>
#include <iostream>
#include <sstream>
#include <iomanip>

// Enabling verification can help if there is memory corruption that is overwriting the MicroOp
// datastructure and you would like to detect when it is happening
//#define ENABLE_VERIFICATION

#ifdef ENABLE_VERIFICATION
# define VERIFY_MICROOP() verify()
#else
# define VERIFY_MICROOP() do {} while (0)
#endif

MicroOp::MicroOp()
#ifdef ENABLE_MICROOP_STRINGS
   : sourceRegisterNames(MAXIMUM_NUMBER_OF_SOURCE_REGISTERS)
   , addressRegisterNames(MAXIMUM_NUMBER_OF_ADDRESS_REGISTERS)
   , destinationRegisterNames(MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS)
#endif
{
   this->uop_type = UOP_INVALID;
   this->instructionOpcode = dl::Decoder::DL_OPCODE_INVALID;
   this->instruction = NULL;

   this->first = false;
   this->last = false;

   this->microOpTypeOffset = 0;
   this->intraInstructionDependencies = 0;

   this->sourceRegistersLength = 0;
   this->addressRegistersLength = 0;
   this->destinationRegistersLength = 0;

   this->interrupt = false;
   this->serializing = false;

   this->branch = false;

   this->m_membar = false;
   this->is_x87 = false;
   this->operand_size = 0;

   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_SOURCE_REGISTERS; i++)
      this->sourceRegisters[i] = dl::Decoder::DL_OPCODE_INVALID;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_ADDRESS_REGISTERS; i++)
      this->addressRegisters[i] = dl::Decoder::DL_OPCODE_INVALID;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS; i++)
      this->destinationRegisters[i] = dl::Decoder::DL_OPCODE_INVALID;

#ifdef ENABLE_MICROOP_STRINGS
   sourceRegisterNames.clear();
   sourceRegisterNames.resize(MAXIMUM_NUMBER_OF_SOURCE_REGISTERS);
   destinationRegisterNames.clear();
   destinationRegisterNames.resize(MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS);
#endif

#ifndef NDEBUG
#ifdef ENABLE_MICROOP_STRINGS
   this->debugInfo = "";
#endif
#endif
}

void MicroOp::makeLoad(uint32_t offset, dl::Decoder::decoder_opcode instructionOpcode, const String& instructionOpcodeName, uint16_t mem_size) {
   this->uop_type = UOP_LOAD;               
   this->microOpTypeOffset = offset;
   this->memoryAccessSize = mem_size;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->instructionOpcode = instructionOpcode;
   this->intraInstructionDependencies = 0;
   this->setTypes();
}

void MicroOp::makeExecute(uint32_t offset, uint32_t num_loads, dl::Decoder::decoder_opcode instructionOpcode, const String& instructionOpcodeName, bool isBranch) {
   this->uop_type = UOP_EXECUTE;
   this->microOpTypeOffset = offset;
   this->intraInstructionDependencies = num_loads;
   this->instructionOpcode = instructionOpcode;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->branch = isBranch;
   this->setTypes();
}

void MicroOp::makeStore(uint32_t offset, uint32_t num_execute, dl::Decoder::decoder_opcode instructionOpcode, const String& instructionOpcodeName, uint16_t mem_size) {
   this->uop_type = UOP_STORE;
   this->microOpTypeOffset = offset;
   this->memoryAccessSize = mem_size;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->instructionOpcode = instructionOpcode;
   this->intraInstructionDependencies = num_execute;
   this->setTypes();
}

void MicroOp::makeDynamic(const String& instructionOpcodeName, uint32_t execLatency) {
   this->uop_type = UOP_EXECUTE;
   this->microOpTypeOffset = 0;
   this->intraInstructionDependencies = 0;
   this->instructionOpcode = dl::Decoder::DL_OPCODE_INVALID;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->branch = false;
   this->setTypes();
}


MicroOp::uop_subtype_t MicroOp::getSubtype_Exec(const MicroOp& uop)
{   
   dl::Decoder *dec = Sim()->getDecoder();

   // Get the uop subtype for the EXEC part of this instruction
   // (ignoring the fact that this particular microop may be a load/store,
   //  used in determining the data type for load/store when calculating bypass delays)
   if (dec->is_branch_opcode(uop.getInstructionOpcode()))
        return UOP_SUBTYPE_BRANCH;

   else if (dec->is_fpvector_addsub_opcode(uop.getInstructionOpcode(), uop.getDecodedInstruction()))
       return UOP_SUBTYPE_FP_ADDSUB;

   else if (dec->is_fpvector_muldiv_opcode(uop.getInstructionOpcode(), uop.getDecodedInstruction()))
       return UOP_SUBTYPE_FP_MULDIV;

   else
       return UOP_SUBTYPE_GENERIC;
}


MicroOp::uop_subtype_t MicroOp::getSubtype(const MicroOp& uop)
{
   // Count all of the ADD/SUB/DIV/LD/ST/BR, and if we have too many, break
   // Count all of the GENERIC insns, and if we have too many (3x-per-cycle), break
   if (uop.isLoad())
      return UOP_SUBTYPE_LOAD;
   else if (uop.isStore())
      return UOP_SUBTYPE_STORE;
   else if (uop.isBranch()) // conditional branches
      return UOP_SUBTYPE_BRANCH;
   else if (uop.isExecute())
      return getSubtype_Exec(uop);
   else
      return UOP_SUBTYPE_GENERIC;
}

String MicroOp::getSubtypeString(uop_subtype_t uop_subtype)
{
   switch(uop_subtype) {
      case UOP_SUBTYPE_FP_ADDSUB:
         return "fp_addsub";
      case UOP_SUBTYPE_FP_MULDIV:
         return "fp_muldiv";
      case UOP_SUBTYPE_LOAD:
         return "load";
      case UOP_SUBTYPE_STORE:
         return "store";
      case UOP_SUBTYPE_GENERIC:
         return "generic";
      case UOP_SUBTYPE_BRANCH:
         return "branch";
      default:
         LOG_ASSERT_ERROR(false, "Unknown UopType %u", uop_subtype);
         return "unknown";
   }
}

bool MicroOp::isFpLoadStore() const
{
   if (isLoad() || isStore())
   {
      switch(getSubtype_Exec(*this))
      {
         case UOP_SUBTYPE_FP_ADDSUB:
         case UOP_SUBTYPE_FP_MULDIV:
            return true;
         default:
            ; // fall through
      }
      if(Sim()->getDecoder()->is_fpvector_ldst_opcode(getInstructionOpcode(), getDecodedInstruction()))
      {
         return true;
      }
   }

   return false;
}


void MicroOp::verify() const {
   LOG_ASSERT_ERROR(uop_subtype == MicroOp::getSubtype(*this), "uop_subtype %u != %u", uop_subtype, MicroOp::getSubtype(*this));
   LOG_ASSERT_ERROR(sourceRegistersLength < MAXIMUM_NUMBER_OF_SOURCE_REGISTERS, "sourceRegistersLength(%d) > MAX(%u)", sourceRegistersLength, MAXIMUM_NUMBER_OF_SOURCE_REGISTERS);
   LOG_ASSERT_ERROR(destinationRegistersLength < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS, "destinationRegistersLength(%u) > MAX(%u)", destinationRegistersLength, MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS);
   for (uint32_t i = 0 ; i < sourceRegistersLength ; i++)
     LOG_ASSERT_ERROR(sourceRegisters[i] < Sim()->getDecoder()->last_reg(), "sourceRegisters[%u] >= DEC_REG_LAST", i);
   for (uint32_t i = 0 ; i < destinationRegistersLength ; i++)
     LOG_ASSERT_ERROR(destinationRegisters[i] < Sim()->getDecoder()->last_reg(), "destinationRegisters[%u] >= DEC_REG_LAST", i);
}

uint32_t MicroOp::getSourceRegistersLength() const {
   VERIFY_MICROOP();
   return this->sourceRegistersLength;
}

dl::Decoder::decoder_reg MicroOp::getSourceRegister(uint32_t index) const {
   VERIFY_MICROOP();
   assert(index < this->sourceRegistersLength);
   return this->sourceRegisters[index];
}

#ifdef ENABLE_MICROOP_STRINGS
const String& MicroOp::getSourceRegisterName(uint32_t index) const {
   VERIFY_MICROOP();
   assert(index < this->sourceRegistersLength);
   return this->sourceRegisterNames[index];
}
#endif

void MicroOp::addSourceRegister(dl::Decoder::decoder_reg registerId, const String& registerName) {
   VERIFY_MICROOP();
   assert(sourceRegistersLength < MAXIMUM_NUMBER_OF_SOURCE_REGISTERS);
// assert(registerId >= 0 && registerId < TOTAL_NUM_REGISTERS);
   sourceRegisters[sourceRegistersLength] = registerId;
#ifdef ENABLE_MICROOP_STRINGS
   sourceRegisterNames[sourceRegistersLength] = registerName;
#endif
   sourceRegistersLength++;
}

uint32_t MicroOp::getAddressRegistersLength() const {
   VERIFY_MICROOP();
   return this->addressRegistersLength;
}

dl::Decoder::decoder_reg MicroOp::getAddressRegister(uint32_t index) const {
   VERIFY_MICROOP();
   assert(index < this->addressRegistersLength);
   return this->addressRegisters[index];
}

#ifdef ENABLE_MICROOP_STRINGS
const String& MicroOp::getAddressRegisterName(uint32_t index) const {
   VERIFY_MICROOP();
   assert(index < this->addressRegistersLength);
   return this->addressRegisterNames[index];
}
#endif

void MicroOp::addAddressRegister(dl::Decoder::decoder_reg registerId, const String& registerName) {
   VERIFY_MICROOP();
   assert(addressRegistersLength < MAXIMUM_NUMBER_OF_ADDRESS_REGISTERS);
// assert(registerId >= 0 && registerId < TOTAL_NUM_REGISTERS);
   addressRegisters[addressRegistersLength] = registerId;
#ifdef ENABLE_MICROOP_STRINGS
   addressRegisterNames[addressRegistersLength] = registerName;
#endif
   addressRegistersLength++;
}

uint32_t MicroOp::getDestinationRegistersLength() const {
   VERIFY_MICROOP();
   return this->destinationRegistersLength;
}

dl::Decoder::decoder_reg MicroOp::getDestinationRegister(uint32_t index) const {
   VERIFY_MICROOP();
   assert(index < this->destinationRegistersLength);
   return this->destinationRegisters[index];
}

#ifdef ENABLE_MICROOP_STRINGS
const String& MicroOp::getDestinationRegisterName(uint32_t index) const {
   VERIFY_MICROOP();
   assert(index < this->destinationRegistersLength);
   return this->destinationRegisterNames[index];
}
#endif

void MicroOp::addDestinationRegister(dl::Decoder::decoder_reg registerId, const String& registerName) {
   VERIFY_MICROOP();
   assert(destinationRegistersLength < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS);
// assert(registerId >= 0 && registerId < TOTAL_NUM_REGISTERS);
   destinationRegisters[destinationRegistersLength] = registerId;
#ifdef ENABLE_MICROOP_STRINGS
   destinationRegisterNames[destinationRegistersLength] = registerName;
#endif
   destinationRegistersLength++;
}

String MicroOp::toString() const {
   std::ostringstream out;
   out << "===============================" << std::endl;
   if (this->isFirst())
      out << "FIRST ";
   if (this->isLast())
      out << "LAST ";
   if (this->uop_type == UOP_LOAD)
      out << " LOAD: " << " ("
         #ifdef ENABLE_MICROOP_STRINGS
         << instructionOpcodeName
         #endif
         << ":0x" << std::hex << instructionOpcode << std::dec << ")" << std::endl;
   else if (this->uop_type == UOP_STORE)
      out << " STORE: " << " ("
         #ifdef ENABLE_MICROOP_STRINGS
         << instructionOpcodeName
         #endif
         << ":0x" << std::hex << instructionOpcode << std::dec << ")" << std::endl;
   else if (this->uop_type == UOP_EXECUTE)
      out << " EXEC ("
         #ifdef ENABLE_MICROOP_STRINGS
         << instructionOpcodeName
         #endif
         << ":0x" << std::hex << instructionOpcode << std::dec << ")" << std::endl;
   else
      out << " INVALID";

   if (this->isBranch()) {
      out << "Branch" << std::endl;
   }
   if (this->isInterrupt()) {
      out << "Interrupt !" << std::endl;
   }
   if (this->isSerializing()) {
      out << "Serializing !" << std::endl;
   }

#ifdef ENABLE_MICROOP_STRINGS
   out << "SREGS: ";
   for(uint32_t i = 0; i < getSourceRegistersLength(); i++)
      out << getSourceRegisterName(i) << " ";
   out << "DREGS: ";
   for(uint32_t i = 0; i < getDestinationRegistersLength(); i++)
      out << getDestinationRegisterName(i) << " ";
   out << std::endl;
#endif

   out << "-------------------------------" << std::endl;

#ifndef NDEBUG
#ifdef ENABLE_MICROOP_STRINGS
   out << debugInfo << std::endl;
#endif
#endif

   return String(out.str().c_str());
}

String MicroOp::toShortString(bool withDisassembly) const
{
   std::ostringstream out;
   if (this->uop_type == UOP_LOAD)
      out << " LOAD  ";
   else if (this->uop_type == UOP_STORE)
      out << " STORE ";
   else if (this->uop_type == UOP_EXECUTE)
      out << " EXEC  ";
   else
      out << " INVALID";
   out << " ("
         #ifdef ENABLE_MICROOP_STRINGS
         << std::left << std::setw(8) << instructionOpcodeName
         #endif
         << ":0x" << std::hex << std::setw(4) << instructionOpcode << std::dec << ")";

   if (withDisassembly)
      out << "  --  " << (this->getInstruction() ? this->getInstruction()->getDisassembly() : "(dynamic)");

   return String(out.str().c_str());
}
