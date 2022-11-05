// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SSE_MMX_TYPES_)
# define _SSE_MMX_TYPES_

#include "sde-base-types.H"
#include <iostream>
#include <mmintrin.h>
#include <emmintrin.h>
#include <xmmintrin.h>

extern "C" {
#include "sde-vreg.h"
}

namespace INSTLIB
{


#define MAX_XMM_REGS 16
#define MAX_BYTES_PER_XMM_REG 16
#define MAX_WORDS_PER_XMM_REG (MAX_BYTES_PER_XMM_REG/2)
#define MAX_DWORDS_PER_XMM_REG (MAX_WORDS_PER_XMM_REG/2)
#define MAX_QWORDS_PER_XMM_REG (MAX_DWORDS_PER_XMM_REG/2)
#define MAX_FLOATS_PER_XMM_REG (MAX_BYTES_PER_XMM_REG/sizeof(float))
#define MAX_DOUBLES_PER_XMM_REG (MAX_BYTES_PER_XMM_REG/sizeof(double))

#define MAX_MMX_REGS 8
#define MAX_BYTES_PER_MMX_REG 8
#define MAX_WORDS_PER_MMX_REG (MAX_BYTES_PER_MMX_REG/2)
#define MAX_DWORDS_PER_MMX_REG (MAX_WORDS_PER_MMX_REG/2)
#define MAX_QWORDS_PER_MMX_REG (MAX_DWORDS_PER_MMX_REG/2)
#define MAX_FLOATS_PER_MMX_REG (MAX_BYTES_PER_MMX_REG/sizeof(float))
#define MAX_DOUBLES_PER_MMX_REG (MAX_BYTES_PER_MMX_REG/sizeof(double))


union xmm_reg_t
{
    sde_xmm_reg_t xmm;
    sde_uint8_t  byte[MAX_BYTES_PER_XMM_REG];
    sde_uint16_t word[MAX_WORDS_PER_XMM_REG];
    sde_uint32_t dword[MAX_DWORDS_PER_XMM_REG];
    sde_uint64_t qword[MAX_QWORDS_PER_XMM_REG];

    sde_int8_t   s_byte[MAX_BYTES_PER_XMM_REG];
    sde_int16_t  s_word[MAX_WORDS_PER_XMM_REG];
    sde_int32_t  s_dword[MAX_DWORDS_PER_XMM_REG];
    sde_int64_t  s_qword[MAX_QWORDS_PER_XMM_REG];

    float  flt[MAX_FLOATS_PER_XMM_REG];
    double dbl[MAX_DOUBLES_PER_XMM_REG];
    
    __m128d xmmpd;
    __m128  xmmps;

    void print(std::ostream& o) const;
    void print_float(std::ostream& o) const;
    void print_double(std::ostream& o) const;

    void print_int8(std::ostream& o) const;
    void print_int16(std::ostream& o) const;
    void print_int32(std::ostream& o) const;
    void print_int64(std::ostream& o) const;
    void print_uint8(std::ostream& o) const;
    void print_uint16(std::ostream& o) const;
    void print_uint32(std::ostream& o) const;
    void print_uint64(std::ostream& o) const;

    void zero() {
        qword[0]=0;
        qword[1]=0;
    }
    

}
#if defined(_WIN32)
    ;    
#else
__attribute__ ((aligned(16)));
#endif

union mmx_reg_t
{
    sde_uint8_t  byte[MAX_BYTES_PER_MMX_REG];
    sde_uint16_t word[MAX_WORDS_PER_MMX_REG];
    sde_uint32_t dword[MAX_DWORDS_PER_MMX_REG];
    sde_uint64_t qword[MAX_QWORDS_PER_MMX_REG];

    sde_int8_t  s_byte[MAX_BYTES_PER_MMX_REG];
    sde_int16_t s_word[MAX_WORDS_PER_MMX_REG];
    sde_int32_t s_dword[MAX_DWORDS_PER_MMX_REG];
    sde_int64_t s_qword[MAX_QWORDS_PER_MMX_REG];

    float  flt[MAX_FLOATS_PER_MMX_REG];
    double dbl[MAX_DOUBLES_PER_MMX_REG];

    void print(std::ostream& o) const;
    void print_float(std::ostream& o) const;
    void print_double(std::ostream& o) const;

    void zero() {
        qword[0]=0;
    }

}
#if defined(_WIN32)
    ;    
#else
__attribute__ ((aligned(16)));
#endif
std::ostream& operator<<(std::ostream& o, const xmm_reg_t& xmm);
std::ostream& operator<<(std::ostream& o, const mmx_reg_t& mmx);

} // namespace
#endif
