#ifndef INSTRUCTION_INFO_HPP_
#define INSTRUCTION_INFO_HPP_

#include "micro_op.h"
#include "instruction.h"

#include "pin.H"

#include <cstring>

class InstructionDecoder {
public:
   InstructionDecoder();
   ~InstructionDecoder();

   class MicroOpRegs {
   public:
      std::set<REG> regs_src;
      std::set<REG> regs_dst;
   };

   class DecodeResult {
   public:
      DecodeResult();
      ~DecodeResult();

      bool hasNext() const;
      MicroOp& next();
   private:

      uint64_t instruction_id;

      INS m_ins;

      bool is_x87, is_serializing;

      int index;
      int numLoads;
      int numExecs;
      int numStores;
      int totalMicroOps;

      uint64_t instruction_pc;
      MicroOp* currentMicroOp;

      std::vector<MicroOpRegs> uop_load, uop_store;
      MicroOpRegs uop_execute;

      String debugInfo;

      bool isLoad(int index);
      int loadNum(int index);
      bool isExec(int index);
      int execNum(int index);
      bool isStore(int index);
      int storeNum(int index);

      void addSrcs(std::set<REG> regs);
      void addDsts(std::set<REG> regs);

      friend class InstructionDecoder;
   };

   DecodeResult& decode(INS ins);

private:
   uint64_t instruction_counter;

   DecodeResult* decode_result;
};


#endif /* INSTRUCTION_INFO_HPP_ */
