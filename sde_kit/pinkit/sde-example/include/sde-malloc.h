// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_MALLOC_H_)
# define _SDE_MALLOC_H_

void* sde_aligned_malloc(sde_uint32_t bytes, sde_uint32_t alignment);
void  sde_aligned_free(void* p);

#endif





