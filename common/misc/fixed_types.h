#ifndef __FIXED_TYPES_H
#define __FIXED_TYPES_H

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS
#include <stdint.h>

typedef uint64_t UInt64;
typedef uint32_t UInt32;
typedef uint16_t UInt16;
typedef uint8_t  UInt8;

typedef int64_t SInt64;
typedef int32_t SInt32;
typedef int16_t SInt16;
typedef int8_t  SInt8;

typedef UInt8 Byte;
typedef UInt8 Boolean;

typedef uintptr_t IntPtr;

typedef uintptr_t carbon_reg_t;

// Carbon core types
typedef SInt32 core_id_t;
typedef SInt32 carbon_thread_t;

#define INVALID_CORE_ID ((core_id_t) -1)
#define INVALID_ADDRESS  ((IntPtr) -1)

#ifdef __cplusplus
// std::string isn't tread-safe when making copies
// See http://gcc.gnu.org/bugzilla/show_bug.cgi?id=21334
#include <ext/vstring.h>
typedef __gnu_cxx::__versa_string<char> String;
#endif /* __cplusplus */

/* Simple atomic operations - no lock required */
#define atomic_inc_int64(target)        __asm__ __volatile__ ("lock incq %0" : "+m" (target) : : "memory")
#define atomic_add_int64(target, value) __asm__ __volatile__ ("lock addq %1, %0" : "+m" (target) : "r" (value) : "memory")

#endif
