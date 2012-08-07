#ifndef __FIXED_TYPES_H
#define __FIXED_TYPES_H

#ifndef __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_CONSTANT_MACROS
# define __STDC_CONSTANT_MACROS
#endif
#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS
#endif

#include <stdint.h>
#include <inttypes.h>

// We define __STDC_LIMIT_MACROS and then include stdint.h
// But if someone else already included stdint.h without first defining __STDC_LIMIT_MACROS,
// UINT64_MAX and friends will not be defined. Test for this here.
#ifndef UINT64_MAX
# error "UINT64_MAX is not defined. Make sure fixed_types.h is first in the include order."
#endif
#ifndef PRId64
# error "PRId64 is not defined. Make sure fixed_types.h is first in the include order."
#endif

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
typedef SInt32 thread_id_t;
typedef SInt32 app_id_t;
typedef SInt32 core_id_t;
typedef SInt32 carbon_thread_t;

#define INVALID_THREAD_ID ((thread_id_t) -1)
#define INVALID_APP_ID ((app_id_t) -1)
#define INVALID_CORE_ID ((core_id_t) -1)
#define INVALID_ADDRESS  ((IntPtr) -1)

#ifdef __cplusplus
// std::string isn't tread-safe when making copies
// See http://gcc.gnu.org/bugzilla/show_bug.cgi?id=21334
#include <ext/vstring.h>
typedef __gnu_cxx::__versa_string<char> String;
#endif /* __cplusplus */

#endif
