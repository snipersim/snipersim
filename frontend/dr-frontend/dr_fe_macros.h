#ifndef _DR_FE_MACROS_H_
#define _DR_FE_MACROS_H_

// Macros used in the DynamoRIO based frontend for Sniper 

// Max number of instructions a buffer can have. It should be big enough to hold all entries between clean calls.
#define MAX_NUM_INS_REFS 8192

// The maximum size of buffer for holding instructions
#define MEM_BUF_SIZE (sizeof(instruction_t) * MAX_NUM_INS_REFS)

// For thread-local storage addressing
#define TLS_SLOT(tls_base, enum_val) (void **)((byte *)(tls_base)+tls_offs+(enum_val))
#define BUF_PTR(tls_base) *(instruction_t **)TLS_SLOT(tls_base, FRONTEND_TLS_OFFS_BUF_PTR)

#endif // _DR_FE_MACROS_H_