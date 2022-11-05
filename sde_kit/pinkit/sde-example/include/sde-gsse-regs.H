// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_GSSE_REGS_H_)
# define _SDE_GSSE_REGS_H_

#include "sde.H"

namespace INSTLIB
{
/// access the ymm state of a particular tid. Note, while the ymm_reg_t is
/// 256b wide, only the upper 128b are valid.
ymm_reg_t sde_get_ymm_upper(THREADID tid, unsigned int regno);

/// set the ymm upper state of a particular tid. Note, while the ymm_reg_t
/// is 256b wide, only the upper 128b are valid.
void sde_set_ymm_upper(THREADID tid, unsigned int regno, ymm_reg_t* v);

// hook the emulator call back in to the SDE register inteface to pinlit
void sde_avx_register_state_callback();


// these use the global emulator object to set/get the ymm registers
void sde_avx_set_reg(CONTEXT* ctxt,
                     THREADID tid,
                     xed_reg_enum_t reg,
                     const sde_ymm_reg_t* yreg);
void sde_avx_get_reg(CONTEXT* ctxt,
                     THREADID tid,
                     xed_reg_enum_t reg,
                     sde_ymm_reg_t* yreg);
} // namespace


//Shim stuff
void  sde_gsse_register_shim_implementation(sde_uint64_t hw_features);

#endif
