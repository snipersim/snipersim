#include "micro_op.h"
#include "instruction.h"

extern "C" {
#include <xed-decoded-inst.h>
}

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
   this->instructionOpcode = XED_ICLASS_INVALID;
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
      this->sourceRegisters[i] = XED_REG_INVALID;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_ADDRESS_REGISTERS; i++)
      this->addressRegisters[i] = XED_REG_INVALID;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS; i++)
      this->destinationRegisters[i] = XED_REG_INVALID;

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

void MicroOp::makeLoad(uint32_t offset, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName, uint16_t mem_size) {
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

void MicroOp::makeExecute(uint32_t offset, uint32_t num_loads, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName, bool isBranch) {
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

void MicroOp::makeStore(uint32_t offset, uint32_t num_execute, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName, uint16_t mem_size) {
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
   this->instructionOpcode = XED_ICLASS_INVALID;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->branch = false;
   this->setTypes();
}


MicroOp::uop_subtype_t MicroOp::getSubtype_Exec(const MicroOp& uop)
{
   // Get the uop subtype for the EXEC part of this instruction
   // (ignoring the fact that this particular microop may be a load/store,
   //  used in determining the data type for load/store when calculating bypass delays)
   switch(uop.getInstructionOpcode())
   {
      case XED_ICLASS_CALL_FAR:
      case XED_ICLASS_CALL_NEAR:
      case XED_ICLASS_JB:
      case XED_ICLASS_JBE:
      case XED_ICLASS_JL:
      case XED_ICLASS_JLE:
      case XED_ICLASS_JMP:
      case XED_ICLASS_JMP_FAR:
      case XED_ICLASS_JNB:
      case XED_ICLASS_JNBE:
      case XED_ICLASS_JNL:
      case XED_ICLASS_JNLE:
      case XED_ICLASS_JNO:
      case XED_ICLASS_JNP:
      case XED_ICLASS_JNS:
      case XED_ICLASS_JNZ:
      case XED_ICLASS_JO:
      case XED_ICLASS_JP:
      case XED_ICLASS_JRCXZ:
      case XED_ICLASS_JS:
      case XED_ICLASS_JZ:
      case XED_ICLASS_RET_FAR:
      case XED_ICLASS_RET_NEAR:
         return UOP_SUBTYPE_BRANCH;
      case XED_ICLASS_ADDPD:
      case XED_ICLASS_ADDPS:
      case XED_ICLASS_ADDSD:
      case XED_ICLASS_ADDSS:
      case XED_ICLASS_ADDSUBPD:
      case XED_ICLASS_ADDSUBPS:
      case XED_ICLASS_SUBPD:
      case XED_ICLASS_SUBPS:
      case XED_ICLASS_SUBSD:
      case XED_ICLASS_SUBSS:
      case XED_ICLASS_VADDPD:
      case XED_ICLASS_VADDPS:
      case XED_ICLASS_VADDSD:
      case XED_ICLASS_VADDSS:
      case XED_ICLASS_VADDSUBPD:
      case XED_ICLASS_VADDSUBPS:
      case XED_ICLASS_VSUBPD:
      case XED_ICLASS_VSUBPS:
      case XED_ICLASS_VSUBSD:
      case XED_ICLASS_VSUBSS:
         return UOP_SUBTYPE_FP_ADDSUB;
      case XED_ICLASS_MULPD:
      case XED_ICLASS_MULPS:
      case XED_ICLASS_MULSD:
      case XED_ICLASS_MULSS:
      case XED_ICLASS_DIVPD:
      case XED_ICLASS_DIVPS:
      case XED_ICLASS_DIVSD:
      case XED_ICLASS_DIVSS:
      case XED_ICLASS_VMULPD:
      case XED_ICLASS_VMULPS:
      case XED_ICLASS_VMULSD:
      case XED_ICLASS_VMULSS:
      case XED_ICLASS_VDIVPD:
      case XED_ICLASS_VDIVPS:
      case XED_ICLASS_VDIVSD:
      case XED_ICLASS_VDIVSS:
         return UOP_SUBTYPE_FP_MULDIV;
      default:
         return UOP_SUBTYPE_GENERIC;
   }
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
      switch(getInstructionOpcode())
      {
         case XED_ICLASS_MOVAPS:
         case XED_ICLASS_MOVAPD:
         case XED_ICLASS_MOVUPS:
         case XED_ICLASS_MOVUPD:
         case XED_ICLASS_MOVSS:
         case XED_ICLASS_MOVSD_XMM:
         case XED_ICLASS_MOVHPS:
         case XED_ICLASS_MOVHPD:
         case XED_ICLASS_MOVLPS:
         case XED_ICLASS_MOVLPD:
         case XED_ICLASS_SHUFPS:
         case XED_ICLASS_SHUFPD:
         case XED_ICLASS_BLENDPS:
         case XED_ICLASS_BLENDPD:
         case XED_ICLASS_MOVDDUP:
         case XED_ICLASS_MOVSHDUP:
         case XED_ICLASS_MOVSLDUP:
         case XED_ICLASS_UNPCKHPS:
         case XED_ICLASS_UNPCKLPS:
         case XED_ICLASS_UNPCKHPD:
         case XED_ICLASS_UNPCKLPD:
         case XED_ICLASS_EXTRACTPS:
         case XED_ICLASS_INSERTPS:
         case XED_ICLASS_CVTPD2PS:
         case XED_ICLASS_CVTSD2SS:
         case XED_ICLASS_CVTPS2PD:
         case XED_ICLASS_CVTSS2SD:
         case XED_ICLASS_CVTDQ2PS:
         case XED_ICLASS_CVTPS2DQ:
         case XED_ICLASS_CVTTPS2DQ:
         case XED_ICLASS_CVTDQ2PD:
         case XED_ICLASS_CVTPD2DQ:
         case XED_ICLASS_CVTTPD2DQ:
         case XED_ICLASS_CVTPI2PS:
         case XED_ICLASS_CVTPS2PI:
         case XED_ICLASS_CVTTPS2PI:
         case XED_ICLASS_CVTPI2PD:
         case XED_ICLASS_CVTPD2PI:
         case XED_ICLASS_CVTTPD2PI:
         case XED_ICLASS_CVTSI2SS:
         case XED_ICLASS_CVTSS2SI:
         case XED_ICLASS_CVTTSS2SI:
         case XED_ICLASS_CVTSI2SD:
         case XED_ICLASS_CVTSD2SI:
         case XED_ICLASS_CVTTSD2SI:
         case XED_ICLASS_COMISS:
         case XED_ICLASS_COMISD:
         case XED_ICLASS_UCOMISS:
         case XED_ICLASS_UCOMISD:
         case XED_ICLASS_MAXSS:
         case XED_ICLASS_MAXSD:
         case XED_ICLASS_MAXPS:
         case XED_ICLASS_MAXPD:
         case XED_ICLASS_MINSS:
         case XED_ICLASS_MINSD:
         case XED_ICLASS_MINPS:
         case XED_ICLASS_MINPD:
         case XED_ICLASS_ROUNDPS:
         case XED_ICLASS_ROUNDPD:
         case XED_ICLASS_DPPS:
         case XED_ICLASS_DPPD:
         case XED_ICLASS_ANDPS:
         case XED_ICLASS_ANDPD:
         case XED_ICLASS_ANDNPS:
         case XED_ICLASS_ANDNPD:
         case XED_ICLASS_ORPS:
         case XED_ICLASS_ORPD:
         case XED_ICLASS_XORPS:
         case XED_ICLASS_XORPD:
            return true;
         default:
            ;
      }
   }

   return false;
}


void MicroOp::verify() const {
   LOG_ASSERT_ERROR(uop_subtype == MicroOp::getSubtype(*this), "uop_subtype %u != %u", uop_subtype, MicroOp::getSubtype(*this));
   LOG_ASSERT_ERROR(sourceRegistersLength < MAXIMUM_NUMBER_OF_SOURCE_REGISTERS, "sourceRegistersLength(%d) > MAX(%u)", sourceRegistersLength, MAXIMUM_NUMBER_OF_SOURCE_REGISTERS);
   LOG_ASSERT_ERROR(destinationRegistersLength < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS, "destinationRegistersLength(%u) > MAX(%u)", destinationRegistersLength, MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS);
   for (uint32_t i = 0 ; i < sourceRegistersLength ; i++)
     LOG_ASSERT_ERROR(sourceRegisters[i] < XED_REG_LAST, "sourceRegisters[%u] >= XED_REG_LAST", i);
   for (uint32_t i = 0 ; i < destinationRegistersLength ; i++)
     LOG_ASSERT_ERROR(destinationRegisters[i] < XED_REG_LAST, "destinationRegisters[%u] >= XED_REG_LAST", i);
}

uint32_t MicroOp::getSourceRegistersLength() const {
   VERIFY_MICROOP();
   return this->sourceRegistersLength;
}

xed_reg_enum_t MicroOp::getSourceRegister(uint32_t index) const {
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

void MicroOp::addSourceRegister(xed_reg_enum_t registerId, const String& registerName) {
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

xed_reg_enum_t MicroOp::getAddressRegister(uint32_t index) const {
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

void MicroOp::addAddressRegister(xed_reg_enum_t registerId, const String& registerName) {
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

xed_reg_enum_t MicroOp::getDestinationRegister(uint32_t index) const {
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

void MicroOp::addDestinationRegister(xed_reg_enum_t registerId, const String& registerName) {
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
