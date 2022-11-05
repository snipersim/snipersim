// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_AVX_TYPES_)
# define _SDE_AVX_TYPES_

#include "sde-base-types.H"
#include "sse-mmx-types.H"
extern "C" {
#include "sde-portability.h"
}
#include <iostream>

#include <mmintrin.h>
#include <xmmintrin.h>
#include <emmintrin.h>

extern "C" {
#include "sde-vreg.h"
}

namespace INSTLIB
{




#define MAX_YMM_REGS 16
#define MAX_BYTES_PER_YMM_REG 32
#define MAX_WORDS_PER_YMM_REG (MAX_BYTES_PER_YMM_REG/2)
#define MAX_DWORDS_PER_YMM_REG (MAX_WORDS_PER_YMM_REG/2)
#define MAX_QWORDS_PER_YMM_REG (MAX_DWORDS_PER_YMM_REG/2)
#define MAX_FLOATS_PER_YMM_REG (MAX_BYTES_PER_YMM_REG/sizeof(float))
#define MAX_DOUBLES_PER_YMM_REG (MAX_BYTES_PER_YMM_REG/sizeof(double))
#define MAX_XMM_PER_YMM_REG     (2)

//forward decl
union xmm_reg_t;

union ymm_reg_t
{
    sde_ymm_reg_t ymm;
    xmm_reg_t xmm[MAX_XMM_PER_YMM_REG]; // two XMM regs fit in a YMM register

    sde_uint8_t  byte[MAX_BYTES_PER_YMM_REG];
    sde_uint16_t word[MAX_WORDS_PER_YMM_REG];
    sde_uint32_t dword[MAX_DWORDS_PER_YMM_REG];
    sde_uint64_t qword[MAX_QWORDS_PER_YMM_REG];

    sde_int8_t   s_byte[MAX_BYTES_PER_YMM_REG];
    sde_int16_t  s_word[MAX_WORDS_PER_YMM_REG];
    sde_int32_t  s_dword[MAX_DWORDS_PER_YMM_REG];
    sde_int64_t  s_qword[MAX_QWORDS_PER_YMM_REG];

    float  flt[MAX_FLOATS_PER_YMM_REG];
    double dbl[MAX_DOUBLES_PER_YMM_REG];

    __m128d xmmpd[MAX_XMM_PER_YMM_REG];
    __m128  xmmps[MAX_XMM_PER_YMM_REG];

    void print(std::ostream& o) const;
    void print_int8(std::ostream& o) const;
    void print_int16(std::ostream& o) const;
    void print_int32(std::ostream& o) const;
    void print_int64(std::ostream& o) const;
    void print_uint8(std::ostream& o) const;
    void print_uint16(std::ostream& o) const;
    void print_uint32(std::ostream& o) const;
    void print_uint64(std::ostream& o) const;
    void print_float(std::ostream& o) const;
    void print_double(std::ostream& o) const;

    void zero() {
#if 0
        __m128i dstv = _mm_setzero_si128();
        _mm_store_ps((float*)xmm,(__m128) dstv);
        _mm_store_ps((float*)(xmm+1),(__m128) dstv);
#else
        for(unsigned int i=0;i< MAX_QWORDS_PER_YMM_REG;i++)
            qword[i] = 0;
#endif
    }

    void zero_upper() {
#if 0
        __m128i dstv = _mm_setzero_si128();
        _mm_store_ps((float*)(xmm+1),(__m128) dstv);
#else
        for(unsigned int i=MAX_QWORDS_PER_XMM_REG;i< MAX_QWORDS_PER_YMM_REG;i++)
            qword[i] = 0;
#endif
    }
    void copy_ymm_upper(const ymm_reg_t* src) { // copy from src
        xmm[1] = src->xmm[1];
    }
    void copy_xmm_to_ymm_upper(const xmm_reg_t* src) { // copy from src
        xmm[1] = *src;
    }

    void copy_xmm(const xmm_reg_t* xmm) { // copy from xmm to ymm
        for(unsigned int i=0;i<MAX_QWORDS_PER_XMM_REG;i++)
            qword[i] = xmm->qword[i];
    }
    void copy_xmm(const sde_xmm_reg_t* xmm) { // copy from xmm to ymm
        for(unsigned int i=0;i<MAX_QWORDS_PER_XMM_REG;i++)
            qword[i] = xmm->qword[i];
    }

    void copy_to_xmm(xmm_reg_t* xmm) const { // copy from ymm-lo to xmm
        for(unsigned int i=0;i<MAX_QWORDS_PER_XMM_REG;i++)
            xmm->qword[i]  = qword[i];
    }

} SDE_ALIGN16;

std::ostream& operator<<(std::ostream& o, const ymm_reg_t& ymm);


} // namespace
#endif
