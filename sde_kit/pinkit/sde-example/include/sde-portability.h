// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#ifndef _SDE_PORTABILITY_H_
# define _SDE_PORTABILITY_H_

#include "sde-c-base-types.h"

#define CAST(x,y) ((x) (y))              /* deprecated: use SDE_CAST() */
#define STATIC_CAST(x,y) ((x) (y))       /* deprecated: use SDE_CAST() */
#define REINTERPRET_CAST(x,y) ((x) (y))  /* deprecated: use SDE_CAST() */
#define SDE_CAST(x,y) ((x) (y))


/* recognize VC98 */
#if defined(_WIN32) && defined(_MSC_VER)
#  if _MSC_VER == 1200
#    define SDE_MSVC6 1
#  endif
#endif
/* recognize MSVSN/8/9/10/11 */
#if defined(_WIN32) && defined(_MSC_VER)
#  if _MSC_VER == 1310
#    define SDE_MSVC7 1
#  endif
#  if (_MSC_VER == 1400) 
#    define SDE_MSVC8 1
#  endif
#  if (_MSC_VER == 1500) 
#    define SDE_MSVC9 1
#  endif
#  if (_MSC_VER == 1600)  //DEV10 MSVS2010
#    define SDE_MSVC10 1
#  endif
#  if (_MSC_VER == 1700)  //DEV11 MSVS2012
#    define SDE_MSVC11 1
#  endif
#  if (_MSC_VER >= 1700)
#    define SDE_MSVC11_OR_LATER 1
#  endif
#endif


/* I've had compatibilty problems here so I'm using a trivial indirection */
#if defined(__GNUC__)
#  if defined(__CYGWIN__)
      /* cygwin's gcc 3.4.4 on windows  complains */
#    define SDE_FMT_X "%lx"
#    define SDE_FMT_08X "%08lx"
#    define SDE_FMT_D "%ld"
#    define SDE_FMT_U "%lu"
#    define SDE_FMT_9U "%9lu"
#  else
#    define SDE_FMT_X "%x"
#    define SDE_FMT_08X "%08x"
#    define SDE_FMT_D "%d"
#    define SDE_FMT_U "%u"
#    define SDE_FMT_9U "%9u"
#  endif
#else
#  define SDE_FMT_X "%x"
#  define SDE_FMT_08X "%08x"
#  define SDE_FMT_D "%d"
#  define SDE_FMT_U "%u"
#  define SDE_FMT_9U "%9u"
#endif

#if defined(__GNUC__) && defined(__LP64__) && !defined(__APPLE__)
# define SDE_FMT_LX "%lx"
# define SDE_FMT_LU "%lu"
# define SDE_FMT_LD "%ld"
# define SDE_FMT_LD9 "%9ld"
# define SDE_FMT_LDn(x) "%" #x "ld"
# define SDE_FMT_LX16 "%016lx"
#else
# define SDE_FMT_LX "%llx"
# define SDE_FMT_LU "%llu"
# define SDE_FMT_LD "%lld"
# define SDE_FMT_LD9 "%9lld"
# define SDE_FMT_LDn(x) "%" #x "lld"
# define SDE_FMT_LX16 "%016llx"
#endif

#if defined(__LP64__)
# define SDE_FMT_ADDRESS SDE_FMT_LX16
#else
# define SDE_FMT_ADDRESS SDE_FMT_08X
#endif

#if defined(SDE_DLL)
//  __declspec(dllexport) works with GNU GCC or MS compilers
# define SDE_DLL_EXPORT __declspec(dllexport)
#else
# define SDE_DLL_EXPORT 
#endif

#if 0
# if defined(_WIN32) && !defined(__GNUC__)
sde_int64_t  strtoll(const char* buf, int p, int base) SDE_DLL_EXPORT;
# endif // defined(_WIN32)
#endif

#if defined(__GNUC__) 
# define SDE_INLINE static inline
# define SDE_NORETURN __attribute__ ((noreturn))
# if __GNUC__ == 2
#   define SDE_NOINLINE 
# else
#   define SDE_NOINLINE __attribute__ ((noinline))
# endif
#else
# define SDE_INLINE static __inline
# if defined(SDE_MSVC6)
#   define SDE_NOINLINE 
# else
#   define SDE_NOINLINE __declspec(noinline)
# endif
# define SDE_NORETURN __declspec(noreturn)
#endif

#if !defined(SDE_ALIGN16)
# if defined(__GNUC__)
#  define SDE_ALIGN16  __attribute__ ((aligned(16)))
# else
#  define SDE_ALIGN16 __declspec(align(16))
# endif
#endif

#if !defined(SDE_ALIGN32)
# if defined(__GNUC__)
#  define SDE_ALIGN32  __attribute__ ((aligned(32)))
# else
#  define SDE_ALIGN32 __declspec(align(32))
# endif
#endif

#if !defined(SDE_ALIGN64)
# if defined(__GNUC__)
#  define SDE_ALIGN64  __attribute__ ((aligned(64)))
# else
#  define SDE_ALIGN64 __declspec(align(64))
# endif
#endif

#if defined(__FreeBSD__)
# define SDE_BSD
#endif
#if defined(__linux__)
# define SDE_LINUX
#endif
#if defined(_MSC_VER)
# define SDE_WINDOWS
#endif
#if defined(__APPLE__)
# define SDE_MAC
#endif

#if !defined(SDE_ALIGN_LOCK)
#if defined(__GNUC__)
# define SDE_ALIGN_LOCK __attribute__ ((aligned(64)))
#else
# define SDE_ALIGN_LOCK __declspec(align(64))
#endif
#endif

#if defined(__LP64__) || defined(__x86_64__) || defined (_M_X64) 
# define SDE_X86_64 1
#endif
#if defined(__i386__) || defined(_M_IX86)
# define SDE_IA32 1
#endif

#if ( defined(__INTEL_COMPILER) && defined(SDE_WINDOWS) ) || defined(_MSC_VER)
# define SDE_MS_COMPATIBLE
#elif ( defined(__INTEL_COMPILER) && !defined(SDE_WINDOWS) ) || defined(__GNUC__)
# define SDE_GNU_COMPATIBLE
#else
# error "Could not find suitable compiler (MS, GNU or Intel)"
#endif

#if defined(SDE_WINDOWS)
# if defined(SDE_X86_64)
#  define SDE_WIN64
# endif
#endif


#if defined(SDE_X86_64)
#  define SDE_FASTCALL
#else
#  if defined(SDE_MS_COMPATIBLE)
#    define SDE_FASTCALL __fastcall
#  elif defined(SDE_GNU_COMPATIBLE)
#    define SDE_FASTCALL __attribute__ ((fastcall))
#  endif
#endif

#if defined(__GNUC__)
#  if defined(__clang__) || defined(SDE_MAC)
#     define SDE_UNUSED __attribute__ ((unused))
#     define SDE_COLD_FUNCTION
#  else
#     define SDE_UNUSED __attribute__ ((unused))
#     define SDE_COLD_FUNCTION __attribute__ ((cold))
#  endif
#else
#  define SDE_UNUSED
#  define SDE_COLD_FUNCTION
#endif

// used for header auto generation.
#define SDE_EMU_FN

#endif  // _SDE_PORTABILITY_H_

////////////////////////////////////////////////////////////////////////////

