// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_AGEN_TYPES_H_)
# define _SDE_AGEN_TYPES_H_

#include "sde-c-base-types.h"

typedef enum {
    SDE_MEMOP_INVALID,
    SDE_MEMOP_LOAD,
    SDE_MEMOP_STORE,
    SDE_MEMOP_LAST
} sde_memop_type_t;

typedef struct {
    sde_addr_t       memea;
    sde_addr_t       memea_trans;
    sde_memop_type_t memop_type;
    sde_uint32_t     bytes_per_ref;
} sde_memop_info_t;

#endif
