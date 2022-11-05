// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_AGEN_H_)
# define _SDE_AGEN_H_

// This is C, not C++

#include "sde-agen-types.h"
/* LRB emulation uses AGEN library but does not have  
 *  XED_ATTRIBUTE_SPECIAL_AGEN_REQUIRED defined. For LRB usage of
 * AGEN functions should be based on XED_ATTRIBUTE_GATHER and 
 * XED_ATTRIBUTE_SCATTER attributes as it used to be */

#define SDE_AGEN

/* sde_agen_init() returns one if SDE has address information for this
 * instruction, 0 otherwise (Pin should be used for address generation
 * information. It will write the number of memory references to the
 * pointer nrefs. */

int sde_agen_init(sde_uint32_t tid,
                  sde_uint32_t* nrefs);  /* output */

/* sde_agen_address() returns the memop information for refnum. The
 * variable refnum is bounded by the value written by sde_agen_init to the
 * nrefs parameter. */
void sde_agen_address(sde_uint32_t tid,
                      sde_uint32_t refnum,
                      sde_memop_info_t* memop_info); /* output */

/* sde_agen_init() returns one if SDE has translated memory for this
 * instruction, 0 otherwise  */

sde_bool_t sde_agen_has_trans_memory(sde_uint32_t tid);  

/* return true if the instruction has agen info */
sde_bool_t sde_agen_is_agen_required(const xed_decoded_inst_t* xedd);

/* return true when instruction is fully emulated with agen */
sde_bool_t sde_agen_is_emu(const xed_decoded_inst_t* xedd);

/* return true when forcing avx2 gather emulation by using agen */
sde_bool_t sde_force_avx2_gather_emulation();

#endif
