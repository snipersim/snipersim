// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_C_FUNCS_H_)
# define _SDE_C_FUNCS_H_
#include "sde-portability.h"
#include "sde-c-base-types.h"
#include "sde-assert.h"


SDE_DLL_EXPORT char sde_hex_nibble_convert(sde_uint32_t b);
SDE_DLL_EXPORT sde_uint64_t sde_get_time();
SDE_DLL_EXPORT sde_uint64_t sde_get_rdtscp(sde_uint32_t *aux);
SDE_DLL_EXPORT char* sde_strdup(const char* s);
SDE_DLL_EXPORT char* sde_strdup_size(const char* s, sde_uint32_t alloc_len);


// works on msvs or gcc
#if defined(SDE_INTERNAL)
# define SDE_FUNCTION __FUNCTION__
#else
# define SDE_FUNCTION ""
#endif

//void sde_assert(bool v, const char* msg=0);
#if defined(SDE_INTERNAL) 
# define sde_assert2(x,msg)  do { if (( x )== 0) \
    sde_internal_assert( #x, msg,  __FILE__, __LINE__, SDE_FUNCTION); } while(0) 
# define sde_assert(x)       do { if (( x )== 0) \
    sde_internal_assert( #x, 0,  __FILE__, __LINE__, SDE_FUNCTION); } while(0) 
# define SDE_MSG(msg)        \
    do { sde_internal_msg( msg,  __FILE__, __LINE__, SDE_FUNCTION); } while(0)
# define SDE_ERROR(msg)      \
    do { sde_internal_assert( msg, 0,  __FILE__, __LINE__, SDE_FUNCTION); } while(0) 
# define SDE_NOW_ERROR(msg)  \
    do { sde_exitnow_pintool_assert( msg, 0,  __FILE__, __LINE__, SDE_FUNCTION); } while(0)
# define SDE_MSG2(msg1,msg2) \
    do { sde_internal_msg2( msg1, msg2, __FILE__, __LINE__, SDE_FUNCTION); } while(0)
#else
# define sde_assert2(x,msg)  do { if (( x )== 0) \
    sde_internal_assert( #x, msg,  "(no-file)", __LINE__, "(no-func)"); } while(0) 
# define sde_assert(x)       do { if (( x )== 0) \
    sde_internal_assert( #x, 0,  "(no-file)", __LINE__, "(no-func)"); } while(0) 
# define SDE_MSG(msg)        \
    do { sde_internal_msg( msg,  "(no-file)", __LINE__, "(no-func)"); } while(0)
# define SDE_ERROR(msg)      \
    do { sde_internal_assert( msg, 0,  "(no-file)", __LINE__, "(no-func)"); } while(0) 
# define SDE_NOW_ERROR(msg)  \
    do { sde_exitnow_pintool_assert( msg, 0,  "(no-file)", __LINE__, "(no-func)"); } while(0) 
# define SDE_MSG2(msg1,msg2) \
    do { sde_internal_msg2( msg1, msg2, "(no-file)", __LINE__, "(no-func)"); } while(0)
#endif


SDE_NORETURN SDE_NOINLINE void sde_internal_assert(const char* s,
                                                   const char* msg,
                                                   const char* file,
                                                   int line,
                                                   const char* function);
SDE_NOINLINE void sde_internal_msg(const char* msg,
                                   const char* file,
                                   int line,
                                   const char* function);
SDE_NOINLINE void sde_internal_msg2(const char* msg,
                                    const char* msg2,
                                    const char* file,
                                    int line,
                                    const char* function);

SDE_NORETURN SDE_NOINLINE void sde_error(const char *s);

#define SDE_NDY() do { sde_assert2(0, "NOT DONE YET");  } while(0)
#define SDE_NOTREACH() do { sde_error( "SHOULD NOT REACH HERE");  } while(0)


SDE_INLINE sde_int16_t sign_extend_bw(sde_int8_t x) { return x; }

SDE_INLINE sde_int32_t sign_extend_bd(sde_int8_t x) { return x; }

SDE_INLINE sde_int64_t sign_extend_bq(sde_int8_t x) { return x; }

SDE_INLINE sde_int32_t sign_extend_wd(sde_int16_t x) { return x; }

SDE_INLINE sde_int64_t sign_extend_wq(sde_int16_t x) { return x; }

SDE_INLINE sde_int64_t sign_extend_dq(sde_int32_t x) { return x; }

SDE_INLINE sde_uint16_t zero_extend_bw(sde_uint8_t x) { return x; }

SDE_INLINE sde_uint32_t zero_extend_bd(sde_uint8_t x) { return x; }

SDE_INLINE sde_uint64_t zero_extend_bq(sde_uint8_t x) { return x; }

SDE_INLINE sde_uint32_t zero_extend_wd(sde_uint16_t x) { return x; }

SDE_INLINE sde_uint64_t zero_extend_wq(sde_uint16_t x) { return x; }

SDE_INLINE sde_uint64_t zero_extend_dq(sde_uint32_t x) { return x; }

//FIXME: remove all uses of sde_get_bit and replace with sde_get_bit32
SDE_INLINE  sde_uint32_t sde_get_bit(sde_uint32_t x, sde_uint32_t b) {
    return ((x>>b)&1);
}
SDE_INLINE  sde_uint8_t  sde_get_bit8(sde_uint8_t x, sde_uint8_t b) {
    return ((x>>b)&1);
}
SDE_INLINE  sde_uint16_t sde_get_bit16(sde_uint16_t x, sde_uint16_t b) {
    return ((x>>b)&1);
}
SDE_INLINE  sde_uint32_t sde_get_bit32(sde_uint32_t x, sde_uint32_t b) {
    return ((x>>b)&1);
}
SDE_INLINE  sde_uint64_t sde_get_bit64(sde_uint64_t x, sde_uint64_t b) {
    return ((x>>b)&1);
}

/* Get the bits [high:low] from a 64-bit unsigned int start is the index
   of the lower bit, the high bit should have index (start + nbits - 1) */
SDE_INLINE sde_uint64_t sde_get_bits(sde_uint64_t val, sde_uint32_t start, sde_uint32_t nbits)
{
    return ((val >> start) & (((sde_uint64_t)1 << nbits) - 1));
}

/*
This is used for optimizations. If the mask is all ones, the operation can be
treated as if it was called with K0
*/

sde_uint32_t sde_lzcount(sde_uint64_t v);
sde_uint32_t sde_ilog2(sde_uint64_t v);

SDE_INLINE  sde_bool_t sde_eff_mask_all_ones(sde_uint8_t nchunks,
                                             sde_uint64_t msk)
{
   static const sde_uint64_t eff_masks_arr[] = 
   {
   0x1,0x3,0xf,0xff,0xffff,0xffffffff,
   0xffffffffffffffffULL
   };

   static const sde_uint8_t max_num_of_elements =
                    sizeof(eff_masks_arr)/sizeof(sde_uint64_t);
    
   sde_uint64_t eff_mask; 
   sde_uint64_t nchunks_64b = SDE_CAST(sde_uint64_t,nchunks); 
   sde_uint32_t eff_mask_index = sde_ilog2(nchunks_64b);
   if (eff_mask_index >= max_num_of_elements) 
        return 0;
   eff_mask = eff_masks_arr[eff_mask_index];     
   return (msk & eff_mask) == eff_mask;      
}


SDE_INLINE sde_uint64_t sde_high_bit_64(sde_uint64_t x) {
    sde_union64_t t;
    t.u64 = x;
    return sde_get_bit64(t.u64,63UL);
}

SDE_INLINE sde_uint32_t sde_high_bit_32(sde_uint32_t x) {
    return sde_get_bit32(x,31);
}
SDE_INLINE sde_uint16_t sde_high_bit_16(sde_uint16_t x) {
    return sde_get_bit16(x,15);
}

SDE_INLINE sde_uint8_t sde_set_bit8(sde_uint8_t x, sde_uint8_t b) {
    return (x|(1<<b));
}

SDE_INLINE sde_uint16_t sde_set_bit16(sde_uint16_t x, sde_uint16_t b) {
    return (x|(1<<b));
}

SDE_INLINE sde_uint32_t sde_set_bit32(sde_uint32_t x, sde_uint32_t b) {
    return (x|(1<<b));
}

SDE_INLINE sde_uint64_t sde_set_bit64(sde_uint64_t x, sde_uint64_t b) {
    return (x|(1ULL<<b));
}

SDE_INLINE sde_uint32_t sde_unset_bit32(sde_uint32_t x, sde_uint32_t b) {
    return (x & ~(1<<b));
}

SDE_INLINE sde_uint64_t sde_unset_bit64(sde_uint64_t x, sde_uint64_t b) {
    return (x & ~(1ULL<<b));
}

sde_uint32_t sde_get_bit_count8(sde_uint8_t x); 
sde_uint32_t sde_get_bit_count16(sde_uint16_t x); 
sde_uint32_t sde_get_bit_count32(sde_uint32_t x); 
sde_uint32_t sde_get_bit_count64(sde_uint64_t x); 

SDE_INLINE sde_uint_t sde_bits_to_bytes_8(sde_uint8_t x) {
    return SDE_CAST(sde_uint_t, x >> 3);
}

SDE_INLINE sde_uint_t sde_bits_to_bytes_16(sde_uint16_t x) {
    return SDE_CAST(sde_uint_t, x >> 3);
}

SDE_INLINE sde_uint_t sde_bits_to_bytes_32(sde_uint32_t x) {
    return SDE_CAST(sde_uint_t, x >> 3);
}

SDE_INLINE sde_uint_t sde_bits_to_bytes_64(sde_uint64_t x) {
    return SDE_CAST(sde_uint_t, x >> 3);
}

///

float sde_round_float_to_float(float x);
double sde_round_double_to_double(double x);

////////////////////////////////////////////////////////////////////////////
// All the set/get rounding mode functions accept values 0...3 and return
// values 0...3.  I did this because of the vagueries of the various OS
// interpretations of the rounding modes.

/* make sure that the x87 rounding mode is set based on the mxcsr for the
 * required platforms */
void sde_ensure_rounding_mode(void);

// just validate that 0...3ness
unsigned int
sde_convert_rounding_mode(unsigned int intel_rnd_mode);

typedef enum sde_rounding_mode_e {
    SDE_ROUNDING_MODE_NEAREST=0,
    SDE_ROUNDING_MODE_DOWN=1,
    SDE_ROUNDING_MODE_UP=2,
    SDE_ROUNDING_MODE_TOZERO=3
} sde_rounding_mode_t;

void sde_set_rounding_mode(sde_uint32_t x);

void  sde_set_x87_cw_intfc(sde_uint16_t* cw);
void  sde_get_x87_cw_intfc(sde_uint16_t* cw);
sde_uint32_t  sde_get_x87_rounding_mode(void);
void sde_set_x87_rounding_mode(sde_uint32_t x);

void sde_set_rounding_truncate(void);
void sde_set_rounding_up(void);
void sde_set_rounding_down(void);
void sde_set_rounding_nearest(void);

// need to be #defines because used in macros
#define    SDE_PAGE_SIZE_BYTES    4096
#define    SDE_CACHE_LINE_BYTES     64
#define    SDE_CACHE_LINE_BYTES_LOG  6
    
SDE_INLINE void* sde_cache_line_align(void* v) {
    sde_addr_t  a = SDE_CAST(sde_addr_t,v) + SDE_CACHE_LINE_BYTES - 1 ;
    a = (a >> SDE_CACHE_LINE_BYTES_LOG) << SDE_CACHE_LINE_BYTES_LOG;
    return SDE_CAST(void*,a);
}

SDE_INLINE sde_addr_t sde_align_to_pageaddr(sde_addr_t addr)
{
    return (addr & (~(SDE_PAGE_SIZE_BYTES - 1)));
}

SDE_INLINE void* sde_align_to_pageptr(void* ptr)
{
    sde_addr_t addr = SDE_CAST(sde_addr_t, ptr);
    return SDE_CAST(void*, sde_align_to_pageaddr(addr));
}

#endif

