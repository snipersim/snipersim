// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_INTRIN_TYPES_H_)
# define _SDE_INTRIN_TYPES_H_

#include "sde-c-base-types.h"
#include "sde-c-funcs.h"
#include "sde-portability.h"

#include <stdio.h>
#include <mmintrin.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <immintrin.h>

#if defined(__GNUC__)
#  if __GNUC__ >= 5 || (__GNUC__ >= 4 && __GNUC_MINOR__ >= 9)
#     define SDE_INTRIN_TYPES_FROM_COMPILER
#  endif
#endif

// Define 256bit types if needed
// Taken from ICC

#if !defined(__AVX__) && !defined(_MSC_VER)  && \
     !defined(SDE_INTRIN_TYPES_FROM_COMPILER)

#if defined(_MSC_VER)
#  define _MMINTRIN_TYPE(X) __declspec(intrin_type) __declspec(align(X))
#else
#  define _MMINTRIN_TYPE(X) __attribute__((aligned(X)))
#endif

/*
 * Intel(R) AVX compiler intrinsics.
 */

#if defined(__ICLANG_COMPILER) ||\
    (defined(SDE_MAC) && ((__clang_major__ == 7 && __clang_minor__ >= 3 &&\
             __clang_patchlevel >=0) || __clang_major__ >= 8))

typedef float    __m256  __attribute__((__vector_size__(32)));
typedef double   __m256d __attribute__((__vector_size__(32)));
typedef long long  __m256i __attribute__((__vector_size__(32)));

#else
typedef union  _MMINTRIN_TYPE(32) __m256 {
    float m256_f32[8];
} __m256;

typedef struct _MMINTRIN_TYPE(32) __m256d {
    double m256d_f64[4];
} __m256d;

typedef union  _MMINTRIN_TYPE(32) __m256i {
#if !defined(_MSC_VER)
    /*
     * To support GNU compatible intialization with initializers list,
     * make first union member to be of int64 type.
     */
    sde_int64_t         m256i_gcc_compatibility[4];
#endif
    sde_int8_t          m256i_i8[32];
    sde_int16_t         m256i_i16[16];
    sde_int32_t         m256i_i32[8];
    sde_int64_t         m256i_i64[4];
    sde_uint8_t         m256i_u8[32];
    sde_uint16_t        m256i_u16[16];
    sde_uint32_t        m256i_u32[8];
    sde_uint64_t        m256i_u64[4];
} __m256i;
#endif /* __ICLANG_COMPILER */

#endif /* !defined(__AVX__) && !defined(_MSC_VER) */

#endif /* _SDE_INTRIN_TYPES_H_ */


