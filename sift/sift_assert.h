#ifndef __SIFT_ASSERT_H
#define __SIFT_ASSERT_H

#include <cassert>

#if defined __cplusplus && __GNUC_PREREQ (2,95)
	#define __ASSERT_VOID_CAST static_cast<void>
#else
	#define __ASSERT_VOID_CAST (void)
#endif

#if !defined(__THROW)
	#if defined __cplusplus && __GNUC_PREREQ (2,8)
		#define __THROW	throw()
	#else
		#define __THROW
	#endif
#endif

#if defined __ASSERT_FUNCTION
#elif defined __cplusplus ? __GNUC_PREREQ (2, 6) : __GNUC_PREREQ (2, 4)
	#define __ASSERT_FUNCTION	__PRETTY_FUNCTION__
#else
	#if defined __STDC_VERSION__ && __STDC_VERSION__ >= 199901L
		#define __ASSERT_FUNCTION	__func__
	#else
		#define __ASSERT_FUNCTION	((const char *) 0)
	#endif
#endif

// Define our own version of assert() with a handler that can be overridden through weak linkage

extern void __sift_assert_fail(__const char *__assertion, __const char *__file, unsigned int __line, __const char *__function) __THROW __attribute__ ((__noreturn__));
extern void __assert_fail(const char *__assertion, const char *__file, unsigned int __line, const char *__function) __THROW __attribute__ ((__noreturn__));

#define sift_assert(expr) ((expr) ? __ASSERT_VOID_CAST (0) : __sift_assert_fail (__STRING(expr), __FILE__, __LINE__, __ASSERT_FUNCTION))

#endif // __SIFT_ASSERT_H
