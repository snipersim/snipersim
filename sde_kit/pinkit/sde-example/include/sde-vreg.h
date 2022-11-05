// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_VREG_H_)
# define _SDE_VREG_H_

/*
 * In order for this to work the types (sde_xmm_reg_t, sde_ymm_reg_t,
 * sde_vreg_t) must be aligned on 16 byte boundaries because this code uses
 * SSE/XMM intrinsics. Use the SDE_ALIGN16 (or SDE_ALIGN64 when declaring
 * variables of this type.  For heap allocated structures containing these
 * types, make sure you get the right alignment for the fields in the
 * structure.
 */

#include "sde-c-base-types.h"
#include "sde-c-funcs.h"
#include "sde-portability.h"
#include "sde-intrin-types.h"

#include <stdio.h>
#include <memory.h>
#include <mmintrin.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <immintrin.h>

/* use an indirection type because the masks might grow over time */
typedef sde_uint64_t  sde_mask_reg_t;
enum {
    SDE_BITS_PER_MASK_REG = 64
};

enum {
    //xmm
    SDE_MAX_BYTES_PER_XMM   = (16),
    SDE_MAX_WORDS_PER_XMM   = (SDE_MAX_BYTES_PER_XMM/2),
    SDE_MAX_DWORDS_PER_XMM  = (SDE_MAX_WORDS_PER_XMM/2),
    SDE_MAX_QWORDS_PER_XMM  = (SDE_MAX_DWORDS_PER_XMM/2),
    SDE_MAX_FLOATS_PER_XMM  = (SDE_MAX_BYTES_PER_XMM/sizeof(float)),
    SDE_MAX_DOUBLES_PER_XMM = (SDE_MAX_BYTES_PER_XMM/sizeof(double)),
    SDE_MAX_FP16_PER_XMM    = SDE_MAX_WORDS_PER_XMM,

    //ymm
    SDE_MAX_BYTES_PER_YMM   = (32),
    SDE_MAX_WORDS_PER_YMM   = (SDE_MAX_BYTES_PER_YMM/2),
    SDE_MAX_DWORDS_PER_YMM  = (SDE_MAX_WORDS_PER_YMM/2),
    SDE_MAX_QWORDS_PER_YMM  = (SDE_MAX_DWORDS_PER_YMM/2),
    SDE_MAX_FLOATS_PER_YMM  = (SDE_MAX_BYTES_PER_YMM/sizeof(float)),
    SDE_MAX_DOUBLES_PER_YMM = (SDE_MAX_BYTES_PER_YMM/sizeof(double)),
    SDE_MAX_XMM_PER_YMM     = (2),

    //zmm
    SDE_MAX_BYTES_PER_ZMM   = (64),
    SDE_MAX_WORDS_PER_ZMM   = (SDE_MAX_BYTES_PER_ZMM/2),
    SDE_MAX_DWORDS_PER_ZMM  = (SDE_MAX_WORDS_PER_ZMM/2),
    SDE_MAX_QWORDS_PER_ZMM  = (SDE_MAX_DWORDS_PER_ZMM/2),
    SDE_MAX_FLOATS_PER_ZMM  = (SDE_MAX_BYTES_PER_ZMM/sizeof(float)),
    SDE_MAX_DOUBLES_PER_ZMM = (SDE_MAX_BYTES_PER_ZMM/sizeof(double)),
    SDE_MAX_XMM_PER_ZMM     = (4),
    SDE_MAX_YMM_PER_ZMM     = (2),


// generic register 
#if defined(SDE_X86_64)
    SDE_MAX_VREGS            = (32),
#else
    SDE_MAX_VREGS            = (8),
#endif
    SDE_MAX_VREGS_AVX2       = (16),
    SDE_MAX_VREGS_32BIT      = (8),
    SDE_MAX_XMM_32BIT        = (8),
    SDE_MAX_YMM_32BIT        = (8),
    SDE_MAX_BYTES_PER_VREG   = (64),
    SDE_MAX_WORDS_PER_VREG   = (SDE_MAX_BYTES_PER_VREG/2),
    SDE_MAX_DWORDS_PER_VREG  = (SDE_MAX_WORDS_PER_VREG/2),
    SDE_MAX_QWORDS_PER_VREG  = (SDE_MAX_DWORDS_PER_VREG/2),
    SDE_MAX_FLOATS_PER_VREG  = (SDE_MAX_BYTES_PER_VREG/sizeof(float)),
    SDE_MAX_DOUBLES_PER_VREG = (SDE_MAX_BYTES_PER_VREG/sizeof(double)),
    SDE_MAX_XMM_PER_VREG     = (4),
    SDE_MAX_YMM_PER_VREG     = (2),

    // lanes
    SDE_MAX_LANES_PER_XMM    = (1),
    SDE_MAX_LANES_PER_YMM    = (2),
    SDE_MAX_LANES_PER_VREG   = (4),
};

typedef union 
{
    sde_uint8_t  byte[SDE_MAX_BYTES_PER_XMM];
    sde_uint16_t word[SDE_MAX_WORDS_PER_XMM];
    sde_uint32_t dword[SDE_MAX_DWORDS_PER_XMM];
    sde_uint64_t qword[SDE_MAX_QWORDS_PER_XMM];

    sde_int8_t   s_byte[SDE_MAX_BYTES_PER_XMM];
    sde_int16_t  s_word[SDE_MAX_WORDS_PER_XMM];
    sde_int32_t  s_dword[SDE_MAX_DWORDS_PER_XMM];
    sde_int64_t  s_qword[SDE_MAX_QWORDS_PER_XMM];

    float  flt[SDE_MAX_FLOATS_PER_XMM];
    double dbl[SDE_MAX_DOUBLES_PER_XMM];

    __m128d xmmpd;
    __m128  xmmps;
    __m128i  xmmi;
} sde_xmm_reg_t;

typedef union 
{
    sde_xmm_reg_t xmm[SDE_MAX_XMM_PER_YMM]; 

    sde_uint8_t  byte[SDE_MAX_BYTES_PER_YMM];
    sde_uint16_t word[SDE_MAX_WORDS_PER_YMM];
    sde_uint32_t dword[SDE_MAX_DWORDS_PER_YMM];
    sde_uint64_t qword[SDE_MAX_QWORDS_PER_YMM];

    sde_int8_t   s_byte[SDE_MAX_BYTES_PER_YMM];
    sde_int16_t  s_word[SDE_MAX_WORDS_PER_YMM];
    sde_int32_t  s_dword[SDE_MAX_DWORDS_PER_YMM];
    sde_int64_t  s_qword[SDE_MAX_QWORDS_PER_YMM];

    float  flt[SDE_MAX_FLOATS_PER_YMM];
    double dbl[SDE_MAX_DOUBLES_PER_YMM];

    __m128d xmmpd[SDE_MAX_XMM_PER_YMM];
    __m128  xmmps[SDE_MAX_XMM_PER_YMM];
    __m128i  xmmi[SDE_MAX_XMM_PER_YMM];

    __m256d ymmpd;
    __m256  ymmps;
    __m256i ymmi;
} sde_ymm_reg_t;

typedef union 
{
    sde_xmm_reg_t xmm[SDE_MAX_XMM_PER_VREG]; 
    sde_ymm_reg_t ymm[SDE_MAX_YMM_PER_VREG]; 

    sde_uint8_t  byte[SDE_MAX_BYTES_PER_VREG];
    sde_uint16_t word[SDE_MAX_WORDS_PER_VREG];
    sde_uint32_t dword[SDE_MAX_DWORDS_PER_VREG];
    sde_uint64_t qword[SDE_MAX_QWORDS_PER_VREG];

    sde_int8_t   s_byte[SDE_MAX_BYTES_PER_VREG];
    sde_int16_t  s_word[SDE_MAX_WORDS_PER_VREG];
    sde_int32_t  s_dword[SDE_MAX_DWORDS_PER_VREG];
    sde_int64_t  s_qword[SDE_MAX_QWORDS_PER_VREG];

    float  flt[SDE_MAX_FLOATS_PER_VREG];
    double dbl[SDE_MAX_DOUBLES_PER_VREG];

    __m128d xmmpd[SDE_MAX_XMM_PER_VREG];
    __m128  xmmps[SDE_MAX_XMM_PER_VREG];
    __m128i  xmmi[SDE_MAX_XMM_PER_VREG];

    __m256d ymmpd[SDE_MAX_YMM_PER_VREG];
    __m256  ymmps[SDE_MAX_YMM_PER_VREG];
    __m256i ymmi [SDE_MAX_YMM_PER_VREG];
} sde_vreg_t;

/* nonchaging pointer to nonchanging data */
void sde_vreg_print(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_int8(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_int16(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_int32(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_int64(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_uint8(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_uint16(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_uint32(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_uint64(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_float(FILE* o, sde_vreg_t const* const v);
void sde_vreg_print_double(FILE* o, sde_vreg_t const* const v);


/* use explicit copy and zero to make the functions inlineable by pin: */

SDE_INLINE void sde_xmm_zero(sde_xmm_reg_t* x) {
    x->qword[0] = 0;
    x->qword[1] = 0;
}

SDE_INLINE void sde_vreg_zero(sde_vreg_t* v) {
    v->qword[0] = 0;
    v->qword[1] = 0;
    v->qword[2] = 0;
    v->qword[3] = 0;
    v->qword[4] = 0;
    v->qword[5] = 0;
    v->qword[6] = 0;
    v->qword[7] = 0;
}

// avx128 upper zero'ing. zero everything else.
SDE_INLINE void sde_vreg_zero_upper_avx128(sde_vreg_t* v) {
    v->qword[2] = 0;
    v->qword[3] = 0;
    v->qword[4] = 0;
    v->qword[5] = 0;
    v->qword[6] = 0;
    v->qword[7] = 0;
}

// avx256 upper zero'ing.
SDE_INLINE void sde_vreg_zero_upper_avx256(sde_vreg_t* v) {
    v->qword[4] = 0;
    v->qword[5] = 0;
    v->qword[6] = 0;
    v->qword[7] = 0;
}

// xmm copy in
SDE_INLINE void sde_vreg_copy_in_xmm(int dst_pos,
                                     sde_vreg_t* dst,
                                     sde_vreg_t const* const src,
                                     int src_pos) {  
    const unsigned int src_q_pos = SDE_MAX_DWORDS_PER_XMM * src_pos;
    const unsigned int dst_q_pos = SDE_MAX_DWORDS_PER_XMM * dst_pos;
        
    dst->qword[dst_q_pos+0] = src->qword[src_q_pos+0];
    dst->qword[dst_q_pos+1] = src->qword[src_q_pos+1];
}
//FIXME: better name?
SDE_INLINE void sde_vreg_copy_in_xmm_type(int dst_pos,
                                          sde_vreg_t* dst,
                                          sde_xmm_reg_t const* const src) { 
    const unsigned int dst_q_pos = SDE_MAX_DWORDS_PER_XMM * dst_pos;

    dst->qword[dst_q_pos+0] = src->qword[0];
    dst->qword[dst_q_pos+1] = src->qword[1];
}


SDE_INLINE void sde_vreg_copy_in_ymm(int dst_pos,
                                     sde_vreg_t* dst,
                                     sde_vreg_t const* const src,
                                     int src_pos) {
    const unsigned int src_q_pos = SDE_MAX_DWORDS_PER_YMM * src_pos;
    const unsigned int dst_q_pos = SDE_MAX_DWORDS_PER_YMM * dst_pos;

    dst->qword[dst_q_pos+0] = src->qword[src_q_pos+0];
    dst->qword[dst_q_pos+1] = src->qword[src_q_pos+1];
    dst->qword[dst_q_pos+2] = src->qword[src_q_pos+2];
    dst->qword[dst_q_pos+3] = src->qword[src_q_pos+3];
}
//FIXME: better name?
SDE_INLINE void sde_vreg_copy_ymm_type(int dst_pos,
                                       sde_vreg_t* dst,
                                       sde_ymm_reg_t const* const src) { 
    const unsigned int dst_q_pos = SDE_MAX_DWORDS_PER_YMM * dst_pos;

    dst->qword[dst_q_pos+0] = src->qword[0];
    dst->qword[dst_q_pos+1] = src->qword[1];
    dst->qword[dst_q_pos+2] = src->qword[2];
    dst->qword[dst_q_pos+3] = src->qword[3];
}


//COPY OUT

SDE_INLINE void sde_vreg_copy_to_xmm(sde_vreg_t const* const src,
                                     sde_xmm_reg_t* xmm)  { 
    __m128 t = _mm_load_ps((float const*)src->flt);
    _mm_store_ps(xmm->flt,t);
}
SDE_INLINE void sde_vreg_copy_to_ymm(sde_vreg_t const* const src,
                                     sde_ymm_reg_t* ymm)  {
    __m128 t0 = _mm_load_ps((float const*)(src->flt)  );
    __m128 t1 = _mm_load_ps((float const*)(src->flt+4));
    _mm_store_ps(ymm->flt  ,t0);
    _mm_store_ps(ymm->flt+4,t1);
}

/* This is for merging incoming Pin state (xmm or ymm inputs) stored in a
   ymm. */
SDE_INLINE void sde_vreg_copy_to_xmm_or_ymm(sde_vreg_t const* const src,
                                            sde_ymm_reg_t* ymm,
                                            int merge)
{
    // copy out zmm-lo to ymm or xmm
    /* if merge==1, copy to xmm. if merge==2, copy to ymm */
    if (merge) { /* 1 or 2 */
        /* copy the first xmm register in either case */
        __m128 t0 = _mm_load_ps((float const*)src->flt  );
        _mm_store_ps(ymm->flt  ,t0);
    }
    if (merge==2) { 
        /* copy the 2nd xmm register */
        __m128 t1 = _mm_load_ps((float const*)src->flt+4);
        _mm_store_ps(ymm->flt+4,t1);
    }
    /* FIXME: when skylake ships or we are native on LRB ... */
}


#endif


