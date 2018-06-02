#ifndef INSTRUCTION_INFOWLIB_HPP_
#define INSTRUCTION_INFOWLIB_HPP_

#include "fixed_types.h"

#include <decoder.h>

//extern "C" {
//#include <xed-decoded-inst.h>
//}

#include <vector>
#include <set>

class Instruction;
class MicroOp;

class InstructionDecoder {
private:
   static void addSrcs(std::set<dl::Decoder::decoder_reg> regs, MicroOp *uop);
   static void addAddrs(std::set<dl::Decoder::decoder_reg> regs, MicroOp *uop);
   static void addDsts(std::set<dl::Decoder::decoder_reg> regs, MicroOp *uop);
   static unsigned int getNumExecs(const dl::DecodedInst *ins, int numLoads, int numStores);
public:
   static const std::vector<const MicroOp*>* decode(IntPtr address, const dl::DecodedInst *ins, Instruction *ins_ptr);
};

#endif /* INSTRUCTION_INFO_HPP_ */
