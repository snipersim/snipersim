// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_REG_INTERFACE_H_)
# define _SDE_REG_INTERFACE_H_

extern "C" {
#include "sde-portability.h"
#include "sde-c-base-types.h"
#include "xed-interface.h"
}
#include <string>


// the type of the context provided by PinLIT to SDE
typedef enum { SDE_CONTEXT_INVALID,
               SDE_CONTEXT_XMM,
               SDE_CONTEXT_YMM, //if running on SNB native
               SDE_CONTEXT_LAST } sde_context_enum_t;

// the type of the context provided by SDE to PinLIT
typedef enum { SDE_OSTATE_INVALID, // Indicates no new state provided
               SDE_OSTATE_AVX1, // sde_ostate_ymm_t
               SDE_OSTATE_AVX3, // sde_ostate_zmm_t
               SDE_OSTATE_LAST } sde_ostate_enum_t;



#define SDE_BYTES_PER_ZMM 64
#define SDE_BYTES_PER_YMM 32
#define SDE_BYTES_PER_MASK 8

#define SDE_MAX_REGS_ZMM             32
#define SDE_MAX_REGS_YMM_SANDYBRIDGE 16
#define SDE_MAX_REGS_MASK             8

#define SDE_MAX_CHKPTR_BOUND_REGS     4
#define SDE_BYTES_PER_CHKPTR_BOUND   16

#define SDE_BYTES_PER_TILECFG        64
#define SDE_BYTES_PER_TILEROW        64
#define SDE_BYTES_PER_TILE         1024
#define SDE_MAX_TILES                 8


// for SKYLAKE
typedef SDE_ALIGN32 struct { 
    sde_uint8_t  zmm[SDE_MAX_REGS_ZMM][SDE_BYTES_PER_ZMM];
    sde_uint8_t mask[SDE_MAX_REGS_MASK][SDE_BYTES_PER_MASK];

    /* This mxcsr replaces the mxcsr from fxsave (Pin's CONTEXT) - new bits
     * are added. */
    sde_uint32_t mxcsr; 
    sde_uint64_t xcr0;

    // some trackers for XSAVEOPT (TBD)...

} sde_ostate_zmm_t;

// for SNB, IVB, HSW, BDW
typedef SDE_ALIGN32 struct { 
    sde_uint8_t ymm[SDE_MAX_REGS_YMM_SANDYBRIDGE][SDE_BYTES_PER_YMM];
    sde_uint64_t xcr0;
    // some trackers for XSAVEOPT (TBD)...
} sde_ostate_ymm_t;


#if defined(SDE_WINDOWS)
typedef SDE_ALIGN32 struct  {
    union {
        sde_ostate_ymm_t y; //snb
        sde_ostate_zmm_t z; //  skylake
    } u;
    sde_ostate_enum_t kind;
} sde_register_output_state_t;
#else
typedef struct  {
    union {
        sde_ostate_ymm_t y; //snb
        sde_ostate_zmm_t z; //  skylake
    } u SDE_ALIGN32;
    sde_ostate_enum_t kind;
} sde_register_output_state_t;
#endif

// PinLIT passes a CONTEXT* and the kind of context to SDE. PinLIT also
// passes a pointer to the output state structure. sde_get_registers() will
// fill that structure in accordingly. 
void sde_get_registers(CONTEXT const* ctxt, 
                       THREADID tid,
                       sde_context_enum_t pin_context_kind,
                       sde_register_output_state_t* output);


// To set the SDE register state. 
void sde_set_register(CONTEXT* ctxt,
                      THREADID tid,
                      xed_reg_enum_t reg,
                      sde_uint8_t* data);

// To get one SDE register state.  You must supply a valid context from Pin
// because Pin holds the lower bits of vector regs. The variable
// "buf_bytes" is the size of the memory buffer pointed to by the "data"
// pointer.
void sde_get_register(CONTEXT* ctxt,
                      THREADID tid,
                      xed_reg_enum_t reg,
                      sde_uint8_t* data,
                      sde_uint_t buf_bytes);


// To get the sde_register_output_state_t as a string for debugging:
std::string sde_dump_register_context(sde_register_output_state_t* output);

#endif
