// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_INSTRUMENT_ORDER_H_)
# define _SDE_INSTRUMENT_ORDER_H_

/* use the call order enum to control the order of pin analysis routines
 * between pre-marsalling, emulation and post marshalling routines.
 * The memory read marsahlling routine may use registers, define the call
 * order of the memory pre-marshalling routine to be after all the registers.
 * additional pre-reg and post-reg call order are used when running on AVX
 * host to control the order before and after the registers pre and post
 * marshalling routine. */

typedef enum {
    // SDE first before every thing
    SDE_CALL_ORDER_FIRST = CALL_ORDER_FIRST-50,

    // pinplay first before 
    PINPLAY_CALL_ORDER_BEFORE_FIRST,
    PINPLAY_CALL_ORDER_REPLAY_THREAD_FIRST_IP,
    PINPLAY_CALL_ORDER_CONTROLLER, // This is the call order of the controller
    PINPLAY_CALL_ORDER_REPLAY_SMC,

    // restore memory and regs before agen and register pre-marshalling
    PINPLAY_CALL_ORDER_REPLAY_FIRST_CHECKPOINT,
    PINPLAY_CALL_ORDER_REPLAY_ASYNC_SIGNALS,
    PINPLAY_CALL_ORDER_REPLAY_CODE_EXCLUSION,
    
    PINPLAY_CALL_ORDER_REPLAY_ICOUNT,
    PINPLAY_CALL_ORDER_REPLAY_TEXT,
    PINPLAY_CALL_ORDER_REPLAY_AGEN_MEMORY,
    
    PINPLAY_CALL_ORDER_LOG_VSYSCALL,
    
    PINPLAY_CALL_ORDER_REPLAY_VSYSCALL, 
    PINPLAY_CALL_ORDER_REPLAY_REGOUT,
    PINPLAY_CALL_ORDER_REPLAY_IMAGE,
    PINPLAY_CALL_ORDER_REPLAY_POST_ICOUNT,
    PINPLAY_CALL_ORDER_REPLAY_MEMORY_TRACE_BEFORE,
    PINPLAY_CALL_ORDER_REPLAY_MEMORY_SYNC,
    PINPLAY_CALL_ORDER_REPLAY_BREAK_POINT,
    PINPLAY_CALL_ORDER_REPLAY_MEMORY,
    PINPLAY_CALL_ORDER_REPLAY_MEMORY_TRACE_AFTER,

    // SDE samples the register state values from pin
    SDE_CALL_ORDER_PRE_REG = CALL_ORDER_FIRST-8,    /* premarshall ymms */
    SDE_CALL_ORDER_REG,                             /* premarshall regs */

    // TSX not friendly instructions handling
    // An instruction that has AGEN might be non-friendly 
    // therefor this must be checked before the SPARSE phase.
    SDE_CALL_ORDER_TSX_NON_FRIENDLY,   

    /* "sparse" is for AGEN operations. They use the address recording
     * mechanism to drive tracing, debugtrace, and SDE's internal memory
     * references. Used by: PL/MPX and Sparse (scatter/gather) */
    SDE_CALL_ORDER_SPARSE, //FIXME: rename from SPARSE to AGEN

    //MPX exceptions are detected in the sparse for BNDSTX/BNDLDX
    //for BNDCL/CU/CN w use SPARSE as well since we need to detect whether 
    //to raise exception or not
    SDE_CALL_ORDER_MPX_EXCEPTION_PREPS,

    SDE_CALL_ORDER_POST_AGEN,

    // "default" befores...
    PINPLAY_CALL_ORDER_BEFORE_DEFAULT = CALL_ORDER_DEFAULT,

    // "last" befores...
    PINPLAY_CALL_ORDER_LOG_PRE_ICOUNT = CALL_ORDER_LAST+3,
    PINPLAY_CALL_ORDER_LOG_FIRST_CHECKPOINT,
    PINPLAY_CALL_ORDER_LOG_TEXT_LOGGING,
    PINPLAY_CALL_ORDER_LOG_SPECIAL_INST,
    PINPLAY_CALL_ORDER_LOG_CODE_EXCLUSION_CALL,
    PINPLAY_CALL_ORDER_LOG_READ_VSYSCALL,

    PINPLAY_CALL_ORDER_LOG_ICOUNT,
    PINPLAY_CALL_ORDER_LOG_SKIP_INSTRUCTION,

    PINPLAY_CALL_ORDER_LOG_MP_MEM_BEFORE,
    PINPLAY_CALL_ORDER_LOG_SMC_MEM_BEFORE,
    PINPLAY_CALL_ORDER_LOG_LOCK_MEM_BEFORE,
    PINPLAY_CALL_ORDER_LOG_MEM_BEFORE,
    
    PINPLAY_CALL_ORDER_LOG_BR_EXCEPTING_INST,
    PINPLAY_CALL_ORDER_LOG_REGOUT,
    PINPLAY_CALL_ORDER_REPLAY_READ_VSYSCALL,
    PINPLAY_CALL_ORDER_LOG_LITCOUNT,
    PINPLAY_CALL_ORDER_REPLAY_SIGNALS,
    PINPLAY_CALL_ORDER_REPLAY_EMULATE_SYSCALL,
    PINPLAY_CALL_ORDER_REPLAY_SKIP_INSTRUCTION,
    PINPLAY_CALL_ORDER_REPLAY_LITCOUNT,
    PINPLAY_CALL_ORDER_BEFORE_LAST,
    
    /* RTM access routine */
    SDE_CALL_ORDER_PRE_MEM = CALL_ORDER_LAST+50,
    /* memory reading for emulation routines */
    SDE_CALL_ORDER_MEM,
    
    SDE_CALL_ORDER_PRE_EMU = CALL_ORDER_LAST+60,
    SDE_CALL_ORDER_EXCEPTION_RAISE,

    /* functional emulation */
    SDE_CALL_ORDER_EMU,

    /* postmarshall regs */
    SDE_CALL_ORDER_POST,

    /* postmarshall ymms */
    SDE_CALL_ORDER_POST_REG
    
}  sde_call_order_t;


/* Call order enumerations for IPOINT_AFTER */
enum sde_after_call_order_t {
    /* call order for sync AVX-512 state - should be first */
    SDE_CALL_ORDER_SYNC = CALL_ORDER_FIRST-1,

    /* Handle cached memory flush in RTM */
    SDE_CALL_ORDER_POST_CACHE = CALL_ORDER_FIRST,

    /* Actions that needs to be done after the tracing actions */
    SDE_CALL_ORDER_LAST = CALL_ORDER_LAST+10
};

/* order for thread start and thread end callbacks */
enum sde_thread_start_order_t {
    SDE_CALL_ORDER_TS_ALLOCATE_STATE = CALL_ORDER_DEFAULT - 10,
    SDE_CALL_ORDER_TS_ALLOCATE_SECONDARY,
    SDE_CALL_ORDER_TS_INIT_STATE,
    SDE_CALL_ORDER_DEFAULT = CALL_ORDER_DEFAULT,
    SDE_CALL_ORDER_TF_INIT_STATE = CALL_ORDER_DEFAULT + 10,
    SDE_CALL_ORDER_TF_DEALLOCATE_SECONDARY,
    SDE_CALL_ORDER_TF_DEALLOCATE_STATE,
};


#endif
