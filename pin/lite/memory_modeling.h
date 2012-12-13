#pragma once

#include "fixed_types.h"
#include "inst_mode.h"
#include "pin.H"

namespace lite
{

void addMemoryModeling(TRACE trace, INS ins, InstMode::inst_mode_t inst_mode);
void handleMemoryRead(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size);
void handleMemoryReadDetailed(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size);
ADDRINT handleMemoryReadFaultinjectionNondetailed(bool is_atomic_update, ADDRINT read_address, ADDRINT *save_ea);
ADDRINT handleMemoryReadFaultinjection(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, ADDRINT read_address, UInt32 read_data_size, UInt32 op_num, ADDRINT *save_ea);
void completeMemoryWrite(bool is_atomic_update, ADDRINT write_address, ADDRINT scratch, UINT32 write_size);
void handleMemoryWrite(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size);
void handleMemoryWriteDetailed(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size);
void handleMemoryWriteFaultinjection(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size);

}
