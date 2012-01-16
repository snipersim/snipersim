#pragma once

#include "fixed_types.h"
#include "pin.H"

namespace lite
{

void addMemoryModeling(TRACE trace, INS ins);
void handleMemoryRead(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size);
void handleMemoryReadDetailed(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr read_address, UInt32 read_data_size);
void handleMemoryWrite(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size);
void handleMemoryWriteDetailed(THREADID thread_id, BOOL executing, ADDRINT eip, bool is_atomic_update, IntPtr write_address, UInt32 write_data_size);
void lightMemoryModel(THREADID thread_id, BOOL write, ADDRINT eip);
void lightMemoryModel2(THREADID thread_id, BOOL write, ADDRINT eip);

}
