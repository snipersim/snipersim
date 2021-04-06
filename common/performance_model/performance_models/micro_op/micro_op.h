#ifndef MICROOP_HPP_
#define MICROOP_HPP_

#include "fixed_types.h"
#include "memory_access.h"
#include "tools.h"
#include "subsecond_time.h"
#include "log.h"
#include "simulator.h"

#include <decoder.h>

#include <vector>

class Instruction;

#define MAX_SRC_REGS 3
#define MAX_MEM_SRC_REGS 2
#define MAX_DEST_REGS 3
#define MAX_MEM_DEST_REGS 2
#define MAX_CF_REGS 11

#define MAXIMUM_NUMBER_OF_SOURCE_REGISTERS (MAX_SRC_REGS + MAX_CF_REGS)
#define MAXIMUM_NUMBER_OF_ADDRESS_REGISTERS MAX_MEM_SRC_REGS
#define MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS (MAX_DEST_REGS + MAX_CF_REGS)
#define MAXIMUM_NUMBER_OF_DEPENDENCIES MAXIMUM_NUMBER_OF_SOURCE_REGISTERS

const uint64_t INVALID_SEQNR = UINT64_MAX;

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

struct MicroOp
{
   MicroOp();

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

   /** This microOp is the first microOp of the instruction. */
   bool first;
   /** This microOp is the last microOp of the instruction. */
   bool last;
   
   dl::Decoder::decoder_opcode instructionOpcode;
   const dl::DecodedInst* decodedInstruction;

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
   dl::Decoder::decoder_reg sourceRegisters[MAXIMUM_NUMBER_OF_SOURCE_REGISTERS];
   /** This field contains the length of the destinationRegisters array. */
   uint32_t addressRegistersLength;
   dl::Decoder::decoder_reg addressRegisters[MAXIMUM_NUMBER_OF_ADDRESS_REGISTERS];
   uint32_t destinationRegistersLength;
   /** This array contains the registers written by this MicroOperation, the integer is an id given by libdisasm64. Only valid for UOP_EXECUTE. */
   dl::Decoder::decoder_reg destinationRegisters[MAXIMUM_NUMBER_OF_DESTINATION_REGISTERS];

#ifdef ENABLE_MICROOP_STRINGS
   std::vector<String> sourceRegisterNames;
   std::vector<String> addressRegisterNames;
   std::vector<String> destinationRegisterNames;
#endif

   /** The instruction pointer. */
   Memory::Access instructionPointer;

   /** Is this microOp an interrupt ? */
   bool interrupt;
   /** Is this microOp serializing ? */
   bool serializing;

   /** Is this instruction a branch ? */
   bool branch;

   /** Debug info about the microOperation. */
#ifdef ENABLE_MICROOP_STRINGS
   String debugInfo;
#endif

   bool m_membar;
   bool is_x87;
   uint16_t operand_size;
   uint16_t memoryAccessSize;

   void makeLoad(uint32_t offset, dl::Decoder::decoder_opcode instructionOpcode, const String& instructionOpcodeName, uint16_t mem_size);
   void makeExecute(uint32_t offset, uint32_t num_loads, dl::Decoder::decoder_opcode instructionOpcode, const String& instructionOpcodeName, bool isBranch);
   void makeStore(uint32_t offset, uint32_t num_execute, dl::Decoder::decoder_opcode instructionOpcode, const String& instructionOpcodeName, uint16_t mem_size);
   void makeDynamic(const String& instructionOpcodeName, uint32_t execLatency);

   static uop_subtype_t getSubtype_Exec(const MicroOp& uop);
   static uop_subtype_t getSubtype(const MicroOp& uop);
   static String getSubtypeString(uop_subtype_t uop_subtype);

   void setTypes() { uop_subtype = getSubtype(*this); }
   uop_subtype_t getSubtype(void) const { return uop_subtype; }

   bool isFpLoadStore() const;
   void setIsX87(bool _is_x87) { is_x87 = _is_x87; }
   bool isX87(void) const { return is_x87; }
   void setOperandSize(int size) { operand_size = size; }
   uint16_t getOperandSize(void) const { return operand_size; }
   uint16_t getMemoryAccessSize(void) const { return memoryAccessSize; }

   void setInstruction(Instruction* instr) { instruction = instr; }
   Instruction* getInstruction(void) const { return instruction; }   
   
   void setDecodedInstruction(const dl::DecodedInst* instr) { decodedInstruction = instr; }
   const dl::DecodedInst* getDecodedInstruction(void) const { return decodedInstruction; }

   dl::Decoder::decoder_opcode getInstructionOpcode() const { return instructionOpcode; }

   void setFirst(bool first) { this->first = first; }
   bool isFirst() const { return this->first; }

   void setLast(bool last) { this->last = last; }
   bool isLast() const { return this->last; }

   void verify() const;

   uint32_t getSourceRegistersLength() const;
   dl::Decoder::decoder_reg getSourceRegister(uint32_t index) const;
   void addSourceRegister(dl::Decoder::decoder_reg registerId, const String& registerName);

   uint32_t getAddressRegistersLength() const;
   dl::Decoder::decoder_reg getAddressRegister(uint32_t index) const;
   void addAddressRegister(dl::Decoder::decoder_reg registerId, const String& registerName);

   uint32_t getDestinationRegistersLength() const;
   dl::Decoder::decoder_reg getDestinationRegister(uint32_t index) const;
   void addDestinationRegister(dl::Decoder::decoder_reg registerId, const String& registerName);

#ifdef ENABLE_MICROOP_STRINGS
   const String& getSourceRegisterName(uint32_t index) const;
   const String& getAddressRegisterName(uint32_t index) const;
   const String& getDestinationRegisterName(uint32_t index) const;
#endif

   const Memory::Access& getInstructionPointer() const { return this->instructionPointer; }
   void setInstructionPointer(const Memory::Access& ip) { this->instructionPointer = ip; }

   bool isBranch() const { return this->branch; }

   bool isInterrupt() const { return this->interrupt; }
   void setInterrupt(bool interrupt) { this->interrupt = interrupt; }

   bool isSerializing() const { return this->serializing; }
   void setSerializing(bool serializing) { this->serializing = serializing; }

   bool isMemBarrier() const { return m_membar; }
   void setMemBarrier(bool membar) { this->m_membar = membar; }
   bool isCacheFlush() const { return Sim()->getDecoder()->is_cache_flush_opcode(this->instructionOpcode); }
   bool isDiv() const { return Sim()->getDecoder()->is_div_opcode(this->instructionOpcode); }
   bool isPause() const { return Sim()->getDecoder()->is_pause_opcode(this->instructionOpcode); }

   bool isLoad() const { return this->uop_type == UOP_LOAD; }
   bool isStore() const { return this->uop_type == UOP_STORE; }

   void setDebugInfo(String debugInfo);
   String toString() const;
   String toShortString(bool withDisassembly = false) const;

   bool isExecute() const { return uop_type == UOP_EXECUTE; }

   bool isWriteback() const { return decodedInstruction->is_writeback(); }

   uop_type_t getType() const { return uop_type; }

#ifdef ENABLE_MICROOP_STRINGS
   const String& getInstructionOpcodeName() { return instructionOpcodeName; }
#endif
};

#endif /* MICROOP_HPP_ */
