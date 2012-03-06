#include "micro_op.h"
#include "windows.h"
#include "lll_info.h"
#include "instruction.h"
#include "instruction_latencies.h"

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

// Statically define the LongLatencyLoad structure
// Provides a single place to get the config-defined cutoff information
LLLInfo lll_info;

void MicroOp::clear() {
   this->uop_type = UOP_INVALID;
   this->instructionOpcode = XED_ICLASS_INVALID;
   this->instruction = NULL;

   this->first = false;
   this->last = false;

   this->microOpTypeOffset = 0;
   this->intraInstructionDependencies = 0;

   this->sourceRegistersLength = 0;
   this->destinationRegistersLength = 0;

   this->dependenciesLength = 0;

   this->execTime = 0;
   this->dispatchTime = 0;
   this->execLatency = 1;
   this->fetchTime = 0;
   this->cpContr = 0;

   // Don't reset the window index: this always stays the same !
   this->sequenceNumber = INVALID_SEQNR;

   this->overlapFlags = 0;

   this->dependent = NO_DEP;

   this->interrupt = false;
   this->serializing = false;

   this->branch = false;
   this->branchTaken = false;
   this->branchMispredicted = false;

   this->cphead = 0;
   this->cptail = 0;
   this->maxProducer = 0;

   this->dCacheHitWhere = HitWhere::UNKNOWN;
   this->iCacheHitWhere = HitWhere::L1I; // Default to an icache hit
   this->iCacheLatency = 0;

   this->m_membar = false;
   this->is_x87 = false;
   this->operand_size = 0;

   this->m_forceLongLatencyLoad = false;

   this->m_period = SubsecondTime::Zero();

   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_SOURCE_REGISTERS; i++)
      this->sourceRegisters[i] = -1;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS; i++)
      this->destinationRegisters[i] = -1;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_DEPENDENCIES; i++)
      this->dependencies[i] = -1;

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

void MicroOp::makeLoad(uint32_t offset, const Memory::Access& loadAccess, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName) {
   this->clear();

   this->uop_type = UOP_LOAD;
   this->microOpTypeOffset = offset;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->instructionOpcode = instructionOpcode;
   this->intraInstructionDependencies = 0;
   this->address = loadAccess;
   this->execLatency = 0;
   this->setTypes();
}

void MicroOp::makeExecute(uint32_t offset, uint32_t num_loads, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName, bool isBranch, bool branchTaken) {
   this->clear();

   this->uop_type = UOP_EXECUTE;
   this->microOpTypeOffset = offset;
   this->intraInstructionDependencies = num_loads;
   this->instructionOpcode = instructionOpcode;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->branch = isBranch;
   this->branchTaken = branchTaken;
   this->execLatency = getInstructionLatency(instructionOpcode);
   this->setTypes();
}

void MicroOp::makeStore(uint32_t offset, uint32_t num_execute, const Memory::Access& storeAccess, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName) {
   this->clear();

   this->uop_type = UOP_STORE;
   this->microOpTypeOffset = offset;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->instructionOpcode = instructionOpcode;
   this->intraInstructionDependencies = num_execute;
   this->address = storeAccess;
   this->execLatency = 0;
   this->setTypes();
}

void MicroOp::makeDynamic(const String& instructionOpcodeName, uint32_t execLatency) {
   this->clear();

   this->uop_type = UOP_EXECUTE;
   this->microOpTypeOffset = 0;
   this->intraInstructionDependencies = 0;
   this->instructionOpcode = XED_ICLASS_INVALID;
#ifdef ENABLE_MICROOP_STRINGS
   this->instructionOpcodeName = instructionOpcodeName;
#endif
   this->branch = false;
   this->branchTaken = false;
   this->execLatency = execLatency;
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

MicroOp::uop_port_t MicroOp::getPort(const MicroOp& uop)
{
   switch(uop.uop_subtype) {
      case UOP_SUBTYPE_FP_ADDSUB:
         return UOP_PORT1;
      case UOP_SUBTYPE_FP_MULDIV:
         return UOP_PORT0;
      case UOP_SUBTYPE_LOAD:
         return UOP_PORT4;
      case UOP_SUBTYPE_STORE:
         return UOP_PORT23;
      case UOP_SUBTYPE_GENERIC:
         switch(uop.getInstructionOpcode()) {
            case XED_ICLASS_IMUL:
               return uop.getOperandSize() == 64 ? UOP_PORT0 : UOP_PORT1;
            case XED_ICLASS_LEA:
               return UOP_PORT1; // 0 on Core2, 1 on Nehalem, 0 (simple) or 1 (complex) on Sandy Bridge
            case XED_ICLASS_MOVSS:
            case XED_ICLASS_MOVSD_XMM:
            case XED_ICLASS_MOVAPS:
            case XED_ICLASS_MOVAPD:
               return UOP_PORT5;
            default:
               return UOP_PORT015;
         }
      case UOP_SUBTYPE_BRANCH:
         return UOP_PORT5;
      default:
         LOG_ASSERT_ERROR(false, "Unknown uop_subtype %u", uop.uop_subtype);
   }
}

void MicroOp::setAlu()
{
   switch(uop_type)
   {
      default:
         uop_alu = UOP_ALU_NONE;
         break;
      case UOP_EXECUTE:
         switch(getInstructionOpcode())
         {
            case XED_ICLASS_DIV:
            case XED_ICLASS_IDIV:
               uop_alu = UOP_ALU_TRIG;
               break;
            default:
               uop_alu = UOP_ALU_NONE;
               break;
         }
   }
}

bool MicroOp::isFpLoadStore(const MicroOp& uop)
{
   if (uop.isLoad() || uop.isStore())
   {
      switch(uop.getInstructionOpcode())
      {
         case XED_ICLASS_MOVSS:
         case XED_ICLASS_MOVSD_XMM:
         case XED_ICLASS_MOVAPS:
         case XED_ICLASS_MOVAPD:
            return true;
         default:
            ;
      }
   }

   return false;
}

MicroOp::uop_bypass_t MicroOp::getBypassType(const MicroOp& uop)
{
   switch(getSubtype(uop))
   {
      case UOP_SUBTYPE_LOAD:
         if (isFpLoadStore(uop))
            return UOP_BYPASS_LOAD_FP;
         break;
      case UOP_SUBTYPE_STORE:
         if (isFpLoadStore(uop))
           return UOP_BYPASS_FP_STORE;
         break;
      case UOP_SUBTYPE_FP_ADDSUB:
      case UOP_SUBTYPE_FP_MULDIV:
         if (uop.isLoad())
            return UOP_BYPASS_LOAD_FP;
         else if (uop.isStore())
            return UOP_BYPASS_FP_STORE;
      default:
         break;
   }
   return UOP_BYPASS_NONE;
}


void MicroOp::setFirst(bool first) {
   VERIFY_MICROOP();
   this->first = first;
}

bool MicroOp::isFirst() const {
   VERIFY_MICROOP();
   return this->first;
}

void MicroOp::setLast(bool last) {
   VERIFY_MICROOP();
   this->last = last;
}

bool MicroOp::isLast() const {
   VERIFY_MICROOP();
   return this->last;
}

void MicroOp::copyTo(MicroOp& destination) const {
   VERIFY_MICROOP();
#ifndef ENABLE_MICROOP_STRINGS
   // Save stuff in destination we shouldn't overwrite
   uint32_t windowIndex = destination.windowIndex;
   uint64_t sequenceNumber = destination.sequenceNumber;
   // Copy everything
   memcpy(&destination, this, sizeof(MicroOp));
   // Restore constants
   destination.windowIndex = windowIndex;
   destination.sequenceNumber = sequenceNumber;
#else
   // Overwrite all fields ! -> excluded: windowIndex and sequenceNumber
   destination.uop_type = this->uop_type;
   destination.uop_subtype = this->uop_subtype;
   destination.uop_port = this->uop_port;
   destination.uop_alu = this->uop_alu;
   destination.first = this->first;
   destination.last = this->last;
   destination.instructionOpcode = this->instructionOpcode;
   destination.instruction = this->instruction;
#ifdef ENABLE_MICROOP_STRINGS
   destination.instructionOpcodeName = this->instructionOpcodeName;
#endif
   destination.microOpTypeOffset = this->microOpTypeOffset;
   destination.intraInstructionDependencies = this->intraInstructionDependencies;
   destination.sourceRegistersLength = this->sourceRegistersLength;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_SOURCE_REGISTERS; i++)
      destination.sourceRegisters[i] = this->sourceRegisters[i];
   destination.destinationRegistersLength = this->destinationRegistersLength;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS; i++)
      destination.destinationRegisters[i] = this->destinationRegisters[i];
   destination.instructionPointer = this->instructionPointer;
   destination.address = this->address;
   destination.dependenciesLength = this->dependenciesLength;
   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_DEPENDENCIES; i++)
      destination.dependencies[i] = this->dependencies[i];
   destination.execTime = this->execTime;
   destination.dispatchTime = this->dispatchTime;
   destination.execLatency = this->execLatency;
   destination.fetchTime = this->fetchTime;
   destination.cpContr = this->cpContr;
   destination.overlapFlags = this->overlapFlags;
   destination.dependent = this->dependent;
   destination.interrupt = this->interrupt;
   destination.serializing = this->serializing;
   destination.branch = this->branch;
   destination.branchTaken = this->branchTaken;
   destination.branchMispredicted = this->branchMispredicted;
// destination.branchPredictorRecord = this->branchPredictorRecord;
#ifndef NDEBUG
#ifdef ENABLE_MICROOP_STRINGS
   destination.debugInfo = this->debugInfo;
#endif
#endif
   destination.cphead = this->cphead;
   destination.cptail = this->cptail;
   destination.maxProducer = this->maxProducer;
   destination.dCacheHitWhere = this->dCacheHitWhere;
   destination.iCacheHitWhere = this->iCacheHitWhere;
   destination.iCacheLatency = this->iCacheLatency;
   destination.m_membar = this->m_membar;
   destination.is_x87 = this->is_x87;
   destination.operand_size = this->operand_size;
   destination.m_forceLongLatencyLoad = this->m_forceLongLatencyLoad;

   // Not clearing/setting the windowIndex -- should we?
   destination.sequenceNumber = INVALID_SEQNR;
   destination.m_period = this->m_period;
#endif
}

void MicroOp::verify() const {
   LOG_ASSERT_ERROR(uop_subtype == MicroOp::getSubtype(*this), "uop_subtype %u != %u", uop_subtype, MicroOp::getSubtype(*this));
   LOG_ASSERT_ERROR(uop_port == MicroOp::getPort(*this), "uop_port %u != %u", uop_port, MicroOp::getPort(*this));
   LOG_ASSERT_ERROR(sourceRegistersLength < MAXIMUM_NUMBER_OF_SOURCE_REGISTERS, "sourceRegistersLength(%d) > MAX(%u)", sourceRegistersLength, MAXIMUM_NUMBER_OF_SOURCE_REGISTERS);
   LOG_ASSERT_ERROR(destinationRegistersLength < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS, "destinationRegistersLength(%u) > MAX(%u)", destinationRegistersLength, MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS);
   LOG_ASSERT_ERROR(dependenciesLength < MAXIMUM_NUMBER_OF_DEPENDENCIES, "dependenciesLength(%u) > MAX(%u)", dependenciesLength, MAXIMUM_NUMBER_OF_DEPENDENCIES);
   for (uint32_t i = 0 ; i < sourceRegistersLength ; i++)
     LOG_ASSERT_ERROR(sourceRegisters[i] < TOTAL_NUM_REGISTERS, "sourceRegisters[%u] >= TOTAL_NUM_REGISTERS", i);
   for (uint32_t i = 0 ; i < destinationRegistersLength ; i++)
     LOG_ASSERT_ERROR(destinationRegisters[i] < TOTAL_NUM_REGISTERS, "destinationRegisters[%u] >= TOTAL_NUM_REGISTERS", i);
   for (uint32_t i = 0 ; i < dependenciesLength ; i++)
     LOG_ASSERT_ERROR(sourceRegisters[i] < TOTAL_NUM_REGISTERS, "sourceRegisters[%u] >= TOTAL_NUM_REGISTERS", i);
}

uint32_t MicroOp::getSourceRegistersLength() const {
   VERIFY_MICROOP();
   return this->sourceRegistersLength;
}

uint8_t MicroOp::getSourceRegister(uint32_t index) const {
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

void MicroOp::addSourceRegister(uint32_t registerId, const String& registerName) {
   VERIFY_MICROOP();
   assert(sourceRegistersLength < MAXIMUM_NUMBER_OF_SOURCE_REGISTERS);
// assert(registerId >= 0 && registerId < TOTAL_NUM_REGISTERS);
   sourceRegisters[sourceRegistersLength] = registerId;
#ifdef ENABLE_MICROOP_STRINGS
   sourceRegisterNames[sourceRegistersLength] = registerName;
#endif
   sourceRegistersLength++;
}

uint32_t MicroOp::getDestinationRegistersLength() const {
   VERIFY_MICROOP();
   return this->destinationRegistersLength;
}

uint8_t MicroOp::getDestinationRegister(uint32_t index) const {
   VERIFY_MICROOP();
   assert(index < this->destinationRegistersLength);
   return this->destinationRegisters[index];
}

#ifdef ENABLE_MICROOP_STRINGS
const String& MicroOp::getDestinationRegisterName(uint32_t index) const {
   VERIFY_MICROOP();
   assert(index >= 0 && index < this->destinationRegistersLength);
   return this->destinationRegisterNames[index];
}
#endif

void MicroOp::addDestinationRegister(uint32_t registerId, const String& registerName) {
   VERIFY_MICROOP();
   assert(destinationRegistersLength < MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS);
// assert(registerId >= 0 && registerId < TOTAL_NUM_REGISTERS);
   destinationRegisters[destinationRegistersLength] = registerId;
#ifdef ENABLE_MICROOP_STRINGS
   destinationRegisterNames[destinationRegistersLength] = registerName;
#endif
   destinationRegistersLength++;
}

uint64_t MicroOp::getDependency(uint32_t index) const {
   VERIFY_MICROOP();
   if (index < this->intraInstructionDependencies) {
      return this->sequenceNumber - this->microOpTypeOffset - this->intraInstructionDependencies + index;
   } else {
      assert((index >= this->intraInstructionDependencies) && ((index - this->intraInstructionDependencies) < this->dependenciesLength));
      return this->dependencies[index-this->intraInstructionDependencies];
   }
}

void MicroOp::addDependency(uint64_t sequenceNumber) {
   VERIFY_MICROOP();
   if (!Tools::contains(dependencies, dependenciesLength, sequenceNumber)) {
      assert(this->dependenciesLength < MAXIMUM_NUMBER_OF_DEPENDENCIES);
      dependencies[dependenciesLength] = sequenceNumber;
      dependenciesLength++;
   }
}

void MicroOp::removeDependency(uint64_t sequenceNumber) {
   VERIFY_MICROOP();
   if (sequenceNumber >= this->sequenceNumber - this->microOpTypeOffset - this->intraInstructionDependencies) {
      // Intra-instruction dependency
      while(intraInstructionDependencies && !(sequenceNumber == this->sequenceNumber - this->microOpTypeOffset - this->intraInstructionDependencies)) {
         // Remove the first intra-instruction dependency, but since this is not the one to be removed, add it to the regular dependencies list
         dependencies[dependenciesLength] = this->sequenceNumber - this->microOpTypeOffset - this->intraInstructionDependencies;
         dependenciesLength++;
         LOG_ASSERT_ERROR(dependenciesLength < MAXIMUM_NUMBER_OF_DEPENDENCIES, "dependenciesLength(%u) > MAX(%u)", dependenciesLength, MAXIMUM_NUMBER_OF_DEPENDENCIES);
         intraInstructionDependencies--;
      }
      // Make sure the exit condition was that the dependency to be removed is now the first one, not that we have exhausted the list
      LOG_ASSERT_ERROR(intraInstructionDependencies > 0, "Something went wrong while removing an intra-instruction dependency");
      // Remove the first intra-instruction dependency by decrementing intraInstructionDependencies
      intraInstructionDependencies--;
   } else {
      // Inter-instruction dependency
      LOG_ASSERT_ERROR(dependenciesLength > 0, "Cannot remove dependency when there are none");
      if (dependencies[dependenciesLength-1] == sequenceNumber)
         ; // sequenceNumber to remove is already at the end, we can just decrement dependenciesLength
      else {
         // Move sequenceNumber to the end of the list
         uint64_t idx = Tools::index(dependencies, dependenciesLength, sequenceNumber);
         LOG_ASSERT_ERROR(idx != UINT64_MAX, "MicroOp dependency list does not contain %ld", sequenceNumber);
         Tools::swap(dependencies, idx, dependenciesLength-1);
      }
      dependenciesLength--;
   }
}

const Memory::Access& MicroOp::getLoadAccess() const {
   VERIFY_MICROOP();
   assert(this->isLoad());
   return this->address;
}

const Memory::Access& MicroOp::getStoreAccess() const {
   VERIFY_MICROOP();
   assert(this->isStore());
   return this->address;
}

void MicroOp::addOverlapFlag(uint32_t flag) {
   VERIFY_MICROOP();
   this->overlapFlags |= flag;
}

bool MicroOp::hasOverlapFlag(uint32_t flag) const {
   VERIFY_MICROOP();
   return this->overlapFlags & flag;
}

void MicroOp::setDebugInfo(String debugInfo) {
   VERIFY_MICROOP();
#ifdef ENABLE_MICROOP_STRINGS
   this->debugInfo = debugInfo;
#endif
}

bool MicroOp::isLongLatencyLoad() const {
   VERIFY_MICROOP();
   LOG_ASSERT_ERROR(isLoad(), "Expected a load instruction.");

   uint32_t cutoff = lll_info.getCutoff();

   // If we are enabled, indicate that this is a long latency load if the latency
   // is above a certain cutoff value
   // Also, honor the forceLLL request if indicated
   return (m_forceLongLatencyLoad | ((cutoff > 0) & (this->execLatency > cutoff)));
}

String MicroOp::toString() const {
   std::ostringstream out;
   out << "===============================" << std::endl;
   if (this->isFirst())
      out << "FIRST ";
   if (this->isLast())
      out << "LAST ";
   out << "Windex: " << this->windowIndex << " SeqNr: ";
   if (sequenceNumber == INVALID_SEQNR)
      out << "INVALID_SEQNR";
   else
      out << sequenceNumber;
   if (this->uop_type == UOP_LOAD)
      out << " LOAD: " << std::hex << address.phys << " ("
         #ifdef ENABLE_MICROOP_STRINGS
         << instructionOpcodeName
         #endif
         << ":0x" << std::hex << instructionOpcode << std::dec << ")" << std::endl;
   else if (this->uop_type == UOP_STORE)
      out << " STORE: " << std::hex << address.phys << " ("
         #ifdef ENABLE_MICROOP_STRINGS
         << instructionOpcodeName
         #endif
         << ":0x" << std::hex << instructionOpcode << std::dec << ")" << std::endl;
   else if (this->uop_type == UOP_EXECUTE)
      out << " EXEC: "
         << execLatency << " ("
         #ifdef ENABLE_MICROOP_STRINGS
         << instructionOpcodeName
         #endif
         << ":0x" << std::hex << instructionOpcode << std::dec << ")" << std::endl;
   else
      out << " INVALID";

   if (this->isBranch()) {
      out << "Branch: ";
      if (this->isBranchTaken())
         out << "Taken ";
      else
         out << "Not Taken ";
      if (this->isBranchMispredicted())
         out << "MISPREDICTED ! " << std::endl;
      else
         out << std::endl;
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

   out << "DEPS: ";
   for(uint32_t i = 0; i < getDependenciesLength(); i++)
   {
      if (getDependency(i) == INVALID_SEQNR)
         out << "INVALID_SEQNR";
      else
         out << std::dec << getDependency(i) << " ";
   }
   out << std::endl;
   out << "Overlaps: " << overlapFlags << std::endl;

   out << "FetchTime:" << fetchTime << " Exectime: " << execTime << " Cphead: " << cphead << " Cptail: " << cptail << " MaxProducer: " << maxProducer << " CurrentIPC: " << (double(72)/(cptail-cphead)) << std::endl;
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
   if (sequenceNumber == INVALID_SEQNR)
      out << "INVALID_SEQNR";
   else
      out << std::setw(8) << sequenceNumber;
   if (this->uop_type == UOP_LOAD)
      out << " LOAD:  " << std::hex << std::setw(8) << address.phys;
   else if (this->uop_type == UOP_STORE)
      out << " STORE: " << std::hex << std::setw(8) << address.phys;
   else if (this->uop_type == UOP_EXECUTE)
      out << " EXEC:  " << std::setw(8) << execLatency;
   else
      out << " INVALID";
   out << " ("
         #ifdef ENABLE_MICROOP_STRINGS
         << std::left << std::setw(8) << instructionOpcodeName
         #endif
         << ":0x" << std::hex << std::setw(4) << instructionOpcode << std::dec << ")";

   out << "  DEPS: ";
   std::ostringstream deps;
   for(uint32_t i = 0; i < getDependenciesLength(); i++)
   {
      if (getDependency(i) == INVALID_SEQNR)
         deps << "INVALID_SEQNR";
      else
         deps << std::dec << getDependency(i) << " ";
   }
   out << std::setw(20) << deps.str();

   if (withDisassembly)
      out << "  --  " << (this->getInstruction() ? this->getInstruction()->getDisassembly() : "(dynamic)");

   return String(out.str().c_str());
}
