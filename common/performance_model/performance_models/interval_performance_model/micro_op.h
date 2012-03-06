#ifndef MICROOP_HPP_
#define MICROOP_HPP_

#include "fixed_types.h"
#include "memory_access.h"
#include "tools.h"
#include "subsecond_time.h"
#include "simulator.h" // graphite simulator + log
#include "log.h"
#include "dynamic_instruction_info.h" // for HitWhere

#include <xed-iclass-enum.h>

class Instruction;

#define MAX_SRC_REGS 3
#define MAX_MEM_SRC_REGS 2
#define MAX_DEST_REGS 3
#define MAX_MEM_DEST_REGS 2
#define MAX_CF_REGS 11

#define MAXIMUM_NUMBER_OF_SOURCE_REGISTERS (MAX_SRC_REGS + MAX_CF_REGS)
#define MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS (MAX_DEST_REGS + MAX_CF_REGS)
#define MAXIMUM_NUMBER_OF_DEPENDENCIES MAXIMUM_NUMBER_OF_SOURCE_REGISTERS

#define TOTAL_NUM_REGISTERS 384 // REG::REG_LAST = 377 for intel64. Only the 64-bit ones are used though, maybe a hashmap is better here.
#define INVALID_SEQNR 0xffffffffffffffff

/**
 * An instruction will be decoded in MicroOperations. There are 3 MicroOperation types: LOAD, EXECUTE AND STORE.
 *
 * Example below: instruction with 2 loads, 1 execute and 2 stores.
 *   +----+----+
 *   |    |    |
 * +----+----+----+----+----+
 * | L1 | L2 | E1 | S1 | S2 |
 * +----+----+----+----+----+
 *             |    |    |
 *             +----+----+
 *
 * E1 depend on L1 and L2: intraInstructionDependencies = 2.
 * S1 and S2 depend on E1: intraInstructionDependencies = 1 (for both S1 and S2).
 *
 * The typeOffset for L1 = 0, L2 = 1, E1 = 0, S1 = 0, S2 = 1.
 *
 * Getting the sequenceNumber of the MicroOperation for intra instruction dependencies:
 * this->sequenceNumber - typeOffset (= sequenceNumber of first microOperation with the current type) - intraInstructionDependencies
 *
 */

// Define to store debug strings inside the MicroOp for MicroOp::toString
//#define ENABLE_MICROOP_STRINGS

struct MicroOp {

#ifdef ENABLE_MICROOP_STRINGS
   MicroOp()
      : sourceRegisterNames(MAXIMUM_NUMBER_OF_SOURCE_REGISTERS)
      , destinationRegisterNames(MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS) {}
#endif

   enum uop_type_t { UOP_INVALID = 0, UOP_LOAD, UOP_EXECUTE, UOP_STORE };
   /** The microOperation type. */
   uop_type_t uop_type;

   enum uop_subtype_t {
      UOP_SUBTYPE_FP_ADDSUB,
      UOP_SUBTYPE_FP_MULDIV,
      UOP_SUBTYPE_LOAD,
      UOP_SUBTYPE_STORE,
      UOP_SUBTYPE_GENERIC,
      UOP_SUBTYPE_BRANCH,
      UOP_SUBTYPE_SIZE,
   };
   uop_subtype_t uop_subtype;

   enum uop_port_t {
      UOP_PORT0,
      UOP_PORT1,
      UOP_PORT23,
      UOP_PORT4,
      UOP_PORT5,
      UOP_PORT015,
      UOP_PORT_SIZE,
   };
   uop_port_t uop_port;

   enum uop_alu_t {
      UOP_ALU_NONE = 0,
      UOP_ALU_TRIG,
      UOP_ALU_SIZE,
   };
   uop_alu_t uop_alu;

   enum uop_bypass_t {
      UOP_BYPASS_NONE,
      UOP_BYPASS_LOAD_FP,
      UOP_BYPASS_FP_STORE,
      UOP_BYPASS_SIZE
   };
   uop_bypass_t uop_bypass;

   /** This microOp is the first microOp of the instruction. */
   bool first;
   /** This microOp is the last microOp of the instruction. */
   bool last;

   xed_iclass_enum_t instructionOpcode;

   Instruction* instruction;

#ifdef ENABLE_MICROOP_STRINGS
   String instructionOpcodeName;
#endif

   /** The typeOffset field contains the offset of the microOp starting from the first microOp with that type. */
   uint32_t microOpTypeOffset;
   /** The intraInstructionDependencies variable gives the number of preceding microOp on which this micrOp depends. This number is counted from the first microOp with that type. */
   uint32_t intraInstructionDependencies;

   /** This field contains the length of the sourceRegisters array. */
   uint32_t sourceRegistersLength;
   /** This array contains the registers read by this MicroOperation, the integer is an id given by libdisasm64. Only valid for UOP_LOAD and UOP_EXECUTE. */
   uint32_t sourceRegisters[MAXIMUM_NUMBER_OF_SOURCE_REGISTERS];
   /** This field contains the length of the destinationRegisters array. */
   uint32_t destinationRegistersLength;
   /** This array contains the registers written by this MicroOperation, the integer is an id given by libdisasm64. Only valid for UOP_EXECUTE. */
   uint32_t destinationRegisters[MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS];

#ifdef ENABLE_MICROOP_STRINGS
   std::vector<String> sourceRegisterNames;
   std::vector<String> destinationRegisterNames;
#endif

   /** The instruction pointer. */
   Memory::Access instructionPointer;

   /** The address is valid for UOP_LOAD and UOP_STORE, it contains the load or store address. */
   Memory::Access address;

   /** This field contains the length of the dependencies array. */
   uint32_t dependenciesLength;
   /** This array contains the dependencies. The uint64_t stored in the array is the sequenceNumber of the dependency. */
   uint64_t dependencies[MAXIMUM_NUMBER_OF_DEPENDENCIES];

   /** The cycle in which the instruction is executed. */
   uint64_t execTime;
   /** The cycle in which the instruction is dispatched. */
   uint64_t dispatchTime;
   /** The latency of the instruction. */
   uint32_t execLatency;
   /** The cycle in which the instruction was fetched. This clock is not synchronized with the simulator clock. */
   uint64_t fetchTime;
   /** The number of cycles this instruction contributes to the critical path. */
   uint64_t cpContr;

   /** The index of the microOperation in the window. */
   uint32_t windowIndex;
   /** The sequence number of the microOperation. Unique ! */
   uint64_t sequenceNumber;

   /** The latency of the microInstruction can be overlapped by a long latency load. The flag states what is overlapped: a icache miss, a branch mispredict or a dcache miss. */
   uint32_t overlapFlags;

   enum {NO_DEP = 0, DATA_DEP = 1, INDEP_MISS = 2};
   /** Used during the block window algorithm, shouldn't be here -> has to be int[doubleWindowSize] in IntervalTimer or Windows. */
   uint32_t dependent;

   /** Is this microOp an interrupt ? */
   bool interrupt;
   /** Is this microOp serializing ? */
   bool serializing;

   /** Is this instruction a branch ? */
   bool branch;
   /** Did a jump occur after this instruction ? */
   bool branchTaken;
   /** Is branch mispredicted ? Only for UOP_EXECUTE and branches. */
   bool branchMispredicted;
// /** The branch predictor record. Only for UOP_EXECUTE and branches. */
// TwolevUpdate branchPredictorRecord;

   /** Debug info about the microOperation. */
#ifdef ENABLE_MICROOP_STRINGS
   String debugInfo;
#endif

   HitWhere::where_t dCacheHitWhere;
   HitWhere::where_t iCacheHitWhere;
   uint32_t iCacheLatency;

   bool m_membar;
   bool is_x87;
   UInt16 operand_size;

   bool m_forceLongLatencyLoad;

   subsecond_time_t m_period;

   //TODO remove these debug variables
   uint64_t cphead;
   uint64_t cptail;
   uint64_t maxProducer;

   enum {ICACHE_OVERLAP = 1, BPRED_OVERLAP = 2, DCACHE_OVERLAP = 4};

   void clear();

   void makeLoad(uint32_t offset, const Memory::Access& loadAccess, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName);
   void makeExecute(uint32_t offset, uint32_t num_loads, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName, bool isBranch, bool jump);
   void makeStore(uint32_t offset, uint32_t num_execute, const Memory::Access& storeAccess, xed_iclass_enum_t instructionOpcode, const String& instructionOpcodeName);
   void makeDynamic(const String& instructionOpcodeName, uint32_t execLatency);

   static uop_subtype_t getSubtype_Exec(const MicroOp& uop);
   static uop_subtype_t getSubtype(const MicroOp& uop);
   static String getSubtypeString(uop_subtype_t uop_subtype);
   static uop_port_t getPort(const MicroOp& uop);
   static uop_bypass_t getBypassType(const MicroOp& uop);
   static bool isFpLoadStore(const MicroOp& uop);
   void setAlu(void);

   void setTypes() { uop_subtype = getSubtype(*this); uop_port = getPort(*this); uop_bypass = getBypassType(*this); setAlu(); }
   uop_subtype_t getSubtype(void) const { return uop_subtype; }
   uop_port_t getPort(void) const { return uop_port; }
   uop_alu_t getAlu(void) const { return uop_alu; }
   uop_bypass_t getBypassType(void) const { return uop_bypass; }

   void setIsX87(bool _is_x87) { is_x87 = _is_x87; }
   bool isX87(void) const { return is_x87; }
   void setOperandSize(int size) { operand_size = size; }
   UInt16 getOperandSize(void) const { return operand_size; }

   void setInstruction(Instruction* instr) { instruction = instr; }
   Instruction* getInstruction(void) const { return instruction; }

   xed_iclass_enum_t getInstructionOpcode() const { return instructionOpcode; }

   void setFirst(bool first);
   bool isFirst() const;

   void setLast(bool last);
   bool isLast() const;

   void copyTo(MicroOp& destination) const;

   void verify() const;

   uint32_t getSourceRegistersLength() const;
   uint8_t getSourceRegister(uint32_t index) const;
   void addSourceRegister(uint32_t registerId, const String& registerName);

   uint32_t getDestinationRegistersLength() const;
   uint8_t getDestinationRegister(uint32_t index) const;
   void addDestinationRegister(uint32_t registerId, const String& registerName);

#ifdef ENABLE_MICROOP_STRINGS
   const String& getSourceRegisterName(uint32_t index) const;
   const String& getDestinationRegisterName(uint32_t index) const;
#endif

   uint32_t getDependenciesLength() const { return this->intraInstructionDependencies + this->dependenciesLength; }
   uint64_t getDependency(uint32_t index) const;
   void addDependency(uint64_t sequenceNumber);
   void removeDependency(uint64_t sequenceNumber);

   void clearDependent() { this->dependent = NO_DEP; }
   bool isDependent() const { return this->dependent == DATA_DEP; }
   void setDataDependent() { this->dependent = DATA_DEP; }
   void setIndependentMiss() { this->dependent = INDEP_MISS; }

   const Memory::Access& getInstructionPointer() const { return this->instructionPointer; }
   void setInstructionPointer(const Memory::Access& ip) { this->instructionPointer = ip; }

   bool isBranch() const { return this->branch; }
   bool isBranchTaken() const { LOG_ASSERT_ERROR(isBranch(), "Expected a branch instruction."); return this->branchTaken; }
   void setBranchTaken(bool _branch_taken) { LOG_ASSERT_ERROR(isBranch(), "Expected a branch instruction."); branchTaken = _branch_taken; }
// TwolevUpdate getBranchPredictorRecord() const;
   bool isBranchMispredicted() const { LOG_ASSERT_ERROR(isBranch(), "Expected a branch instruction."); return this->branchMispredicted; }
// void setBranchPredictorRecord(TwolevUpdate record) { this->branchPredictorRecord = record; }
   void setBranchMispredicted(bool mispredicted) { this->branchMispredicted = mispredicted; }

   bool isInterrupt() const { return this->interrupt; }
   void setInterrupt(bool interrupt) { this->interrupt = interrupt; }

   bool isSerializing() const { return this->serializing; }
   void setSerializing(bool serializing) { this->serializing = serializing; }

   bool isMemBarrier() const { return m_membar; }
   void setMemBarrier(bool membar) { this->m_membar = membar; }
   bool isCacheFlush() const {
      // FIXME: Old decoder only listed these three instructions, but there may be more (INVEPT, INVLPGA, INVVPID)
      //        Currently, no-one is using isCacheFlush though.
      return this->instructionOpcode == XED_ICLASS_WBINVD
          || this->instructionOpcode == XED_ICLASS_INVD
          || this->instructionOpcode == XED_ICLASS_INVLPG;
   }
   bool isDiv() const { return this->instructionOpcode == XED_ICLASS_DIV || this->instructionOpcode == XED_ICLASS_IDIV; }
   bool isPause() const { return this->instructionOpcode == XED_ICLASS_PAUSE; }

   bool isLoad() const { return this->uop_type == UOP_LOAD; }
   const Memory::Access& getLoadAccess() const;
   bool isLongLatencyLoad() const;
   bool isStore() const { return this->uop_type == UOP_STORE; }
   const Memory::Access& getStoreAccess() const;

   void addOverlapFlag(uint32_t flag);
   bool hasOverlapFlag(uint32_t flag) const;

   void setDebugInfo(String debugInfo);
   String toString() const;
   String toShortString(bool withDisassembly = false) const;

   uint64_t getExecTime() const { return this->execTime; }
   void setExecTime(uint64_t time) { this->execTime = time; }

   uint64_t getDispatchTime() const { return this->dispatchTime; }
   void setDispatchTime(uint64_t time) { this->dispatchTime = time; }

   uint64_t getFetchTime() const { return this->fetchTime; }
   void setFetchTime(uint64_t time) { this->fetchTime = time; }

   uint64_t getCpContr() const { return this->cpContr; }
   void setCpContr(uint64_t cpContr) { this->cpContr = cpContr; }

   uint32_t getExecLatency() const { return this->execLatency; }
   void setExecLatency(uint32_t latency) { this->execLatency = latency; }

   uint64_t getSequenceNumber() const { return this->sequenceNumber; }
   void setSequenceNumber(uint64_t number) { this->sequenceNumber = number; }

   uint32_t getWindowIndex() const { return this->windowIndex; }
   void setWindowIndex(uint32_t index) { this->windowIndex = index; }

   HitWhere::where_t getDCacheHitWhere() const { return dCacheHitWhere; }
   void setDCacheHitWhere(HitWhere::where_t _hitWhere) { dCacheHitWhere = _hitWhere; }

   HitWhere::where_t getICacheHitWhere() const { return iCacheHitWhere; }
   void setICacheHitWhere(HitWhere::where_t _hitWhere) { iCacheHitWhere = _hitWhere; }

   uint32_t getICacheLatency() const { return iCacheLatency; }
   void setICacheLatency(uint32_t _latency) { iCacheLatency = _latency; };

   bool isExecute() const { return uop_type == UOP_EXECUTE; };

   uop_type_t getType() const { return uop_type; }

   void setAddress(const Memory::Access& loadAccess) { this->address = loadAccess; }
   const Memory::Access& getAddress(void) { return this->address; }

#ifdef ENABLE_MICROOP_STRINGS
   const String& getInstructionOpcodeName() { return instructionOpcodeName; }
#endif

   void setForceLongLatencyLoad(bool forceLLL) { m_forceLongLatencyLoad = forceLLL; }

   SubsecondTime getPeriod() const { LOG_ASSERT_ERROR(m_period != SubsecondTime::Zero(), "MicroOp Period is == SubsecondTime::Zero()"); return m_period; }
   void setPeriod(SubsecondTime _period) { LOG_ASSERT_ERROR(_period != SubsecondTime::Zero(), "MicroOp Period is == SubsecondTime::Zero()"); m_period = _period; }
};

#endif /* MICROOP_HPP_ */
