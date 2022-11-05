// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_MODEL_SPECIFIC_FEATURES_H_)
# define _SDE_MODEL_SPECIFIC_FEATURES_H_

#include "xed-interface.h"

int sde_is_sparse_operation(const xed_decoded_inst_t* xedd);
int sde_is_gather(const xed_decoded_inst_t* xedd);
int sde_is_scatter(const xed_decoded_inst_t* xedd);
int sde_is_sse(const xed_decoded_inst_t* xedd);
int sde_is_amx(const xed_decoded_inst_t* xedd);
int sde_cldemote_enabled();
void sde_cldemote_set(int enabled);

#endif
