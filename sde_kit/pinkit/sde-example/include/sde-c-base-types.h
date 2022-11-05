// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_C_BASE_TYPES_H_)
# define _SDE_C_BASE_TYPES_H_

#include <emmintrin.h>

#if defined(_WIN32) && !defined(__GNUC__)
/* PinCRT provides stdint.h for Windows */
#  include <stdint.h>
#  define sde_uint8_t  unsigned __int8
#  define sde_uint16_t unsigned __int16
#  define sde_uint32_t unsigned __int32
#  define sde_uint64_t unsigned __int64
#  define sde_int8_t  __int8
#  define sde_int16_t __int16
#  define sde_int32_t __int32
#  define sde_int64_t __int64
#  define sde_uint_t unsigned int
#  define sde_bool_t unsigned int
#elif defined(__GNUC__)
#  include <stdint.h>
   typedef uint8_t  sde_uint8_t;
   typedef uint16_t sde_uint16_t;
   typedef uint32_t sde_uint32_t; 
   typedef uint64_t sde_uint64_t;
                            
   typedef int8_t   sde_int8_t;  
   typedef int16_t  sde_int16_t; 
   typedef int32_t  sde_int32_t; 
   typedef int64_t  sde_int64_t;
   typedef unsigned int sde_uint_t;
   typedef unsigned int sde_bool_t;
#else
# error "Unknown platform. Need GNU or WIN32/WIN64"
#endif

typedef union {
  struct {
    sde_uint8_t b0;  /*low 8 bits*/
    sde_uint8_t b1;
    sde_uint8_t b2;
    sde_uint8_t b3;  /*high 8 bits*/
  } b;
  struct {
    sde_int8_t b0;  /*low 8 bits*/
    sde_int8_t b1;
    sde_int8_t b2;
    sde_int8_t b3;  /*high 8 bits*/
  } sb;
  struct {
    sde_uint16_t w0; /*low 16 bits*/
    sde_uint16_t w1; /*high 16 bits*/
  } w;
  sde_int32_t  i32;
  sde_uint32_t u32;
  float f;
} sde_union32_t ;

typedef union {
  struct {
    sde_uint8_t b0;  /*low 8 bits*/
    sde_uint8_t b1;
    sde_uint8_t b2;
    sde_uint8_t b3;
    sde_uint8_t b4;
    sde_uint8_t b5;
    sde_uint8_t b6;
    sde_uint8_t b7;  /*high 8 bits*/
  } b;
  struct {
    sde_uint16_t w0; /*low 16 bits*/
    sde_uint16_t w1;
    sde_uint16_t w2;
    sde_uint16_t w3; /*high 16 bits*/
  } w;
  struct {
    sde_int32_t lo32;
    sde_int32_t hi32;
  } s;
  struct {
    sde_uint32_t lo32;
    sde_uint32_t hi32;
  } u;
  struct {
    float f0;
    float f1;
  } f;
  struct {
#if defined(SDE_IA32)
    void *ptr;
    void *pad;
#else
    void *ptr;
#endif
  } p;
  sde_uint64_t u64;
  sde_int64_t i64;
  double d;
} sde_union64_t ;


#if defined(__LP64__)  || defined(_M_X64)

// 128 bits union
typedef union {
    struct {
        sde_uint8_t b0;  /*low*/
        sde_uint8_t b1;
        sde_uint8_t b2;
        sde_uint8_t b3;
        sde_uint8_t b4;
        sde_uint8_t b5;
        sde_uint8_t b6;
        sde_uint8_t b7;  
        sde_uint8_t b8;  
        sde_uint8_t b9;
        sde_uint8_t b10;
        sde_uint8_t b11;
        sde_uint8_t b12;
        sde_uint8_t b13;
        sde_uint8_t b14;
        sde_uint8_t b15;  /*high */
    } b;
    struct {
        sde_uint16_t w0; /*low word */
        sde_uint16_t w1;
        sde_uint16_t w2;
        sde_uint16_t w3; 
        sde_uint16_t w4; 
        sde_uint16_t w5;
        sde_uint16_t w6;
        sde_uint16_t w7; /*high word */
    } w;
    struct {
        sde_int32_t dw0; /* low dword */
        sde_int32_t dw1;
        sde_int32_t dw2;
        sde_int32_t dw3; /* high dword */
    } dw;
    struct {
        sde_int64_t lo64;
        sde_int64_t hi64;
    } s;
    struct {
        sde_uint64_t lo64;
        sde_uint64_t hi64;
    } u;

    // Intrinsics
    __m128i i128; 

} sde_union128_t ;
#endif

#if defined(__LP64__)  || defined(_M_X64)
  typedef sde_uint64_t sde_addr_t;
#else
  typedef sde_uint32_t sde_addr_t;
#endif


#endif // _SDE_BASE_TYPES_H_
