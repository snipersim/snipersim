// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_CPUID_INTF_H_)
# define _SDE_CPUID_INTF_H_

#include "sde-cpuid-rec.h"

/* SDE->PINLIT INTERFACE */

/* get the number of cpuid data rows */
sde_uint32_t sde_cpuid_get_nrows(void);

/* fill in *r with content from the specified row. row must be less than
 * the return value from sde_cpuid_get_nrows().
 */
void sde_cpuid_get_rows(sde_uint32_t row, sde_cpuid_row_t* r);

/* get the number of cpuid data rows */
sde_uint32_t sde_cpuid_get_nrows(void);

/* fill in *r with content from the specified row. row must be less than
 * the return value from sde_cpuid_get_nrows().
 */
void sde_cpuid_get_rows(sde_uint32_t row, sde_cpuid_row_t* r);

/* update the row with the content of the specified row. row must be less than
 * the return value from sde_cpuid_get_nrows().
 */
void sde_cpuid_set_rows(sde_uint32_t row, sde_cpuid_row_t* r);

#endif







