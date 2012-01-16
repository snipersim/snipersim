#ifndef __REDIRECT_MEMORY_H__
#define __REDIRECT_MEMORY_H__

#include "core.h"
#include "pin.H"
#include "pin_memory_manager.h"

bool rewriteStringOp (INS ins);
bool rewriteStackOp (INS ins);
void rewriteMemOp (INS ins);

VOID emuCMPSBIns (ADDRINT eip, CONTEXT *ctxt, ADDRINT next_gip, bool has_rep_prefix);
VOID emuSCASBIns (ADDRINT eip, CONTEXT *ctxt, ADDRINT next_gip, bool has_rep_prefix);

ADDRINT emuPushValue(ADDRINT eip, ADDRINT tgt_esp, ADDRINT value, ADDRINT write_size);
ADDRINT emuPushMem(ADDRINT eip, ADDRINT tgt_esp, ADDRINT operand_ea, ADDRINT size);
ADDRINT emuPopReg(ADDRINT eip, ADDRINT tgt_esp, ADDRINT *reg, ADDRINT read_size);
ADDRINT emuPopMem(ADDRINT eip, ADDRINT tgt_esp, ADDRINT operand_ea, ADDRINT size);
ADDRINT emuCallMem(ADDRINT eip, ADDRINT *tgt_esp, ADDRINT *tgt_eax, ADDRINT next_ip, ADDRINT operand_ea, ADDRINT read_size, ADDRINT write_size);
ADDRINT emuCallRegOrImm(ADDRINT eip, ADDRINT *tgt_esp, ADDRINT *tgt_eax, ADDRINT next_ip, ADDRINT br_tgt_ip, ADDRINT write_size);
ADDRINT emuRet(ADDRINT eip, ADDRINT *tgt_esp, UINT32 imm, ADDRINT read_size, UInt32 modeled);
ADDRINT emuLeave(ADDRINT eip, ADDRINT tgt_esp, ADDRINT *tgt_ebp, ADDRINT read_size);
ADDRINT redirectPushf (ADDRINT eip, ADDRINT tgt_esp, ADDRINT size );
ADDRINT completePushf (ADDRINT eip, ADDRINT esp, ADDRINT size);
ADDRINT redirectPopf (ADDRINT eip, ADDRINT tgt_esp, ADDRINT size);
ADDRINT completePopf (ADDRINT eip, ADDRINT esp, ADDRINT size);

ADDRINT redirectMemOp (ADDRINT eip, bool has_lock_prefix, ADDRINT tgt_ea, ADDRINT size, UInt32 op_num, UInt32 is_read);
ADDRINT redirectMemOpSaveEa(ADDRINT ea);
VOID completeMemWrite (ADDRINT eip, bool has_lock_prefix, ADDRINT tgt_ea, ADDRINT size, UInt32 op_num);

void memOp (ADDRINT eip, Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char *data_buffer, UInt32 data_size);

#endif /* __REDIRECT_MEMORY_H__ */
