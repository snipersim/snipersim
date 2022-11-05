// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_CPUID_REC_H_)
# define _SDE_CPUID_REC_H_

#include "sde-c-base-types.h"

typedef struct {
    sde_uint32_t eax_in;
    sde_uint32_t ecx_in;
    sde_bool_t ecx_dontcare;

    sde_uint32_t eax_out;
    sde_uint32_t ebx_out;
    sde_uint32_t ecx_out;
    sde_uint32_t edx_out;

} sde_cpuid_row_t; 


#endif
