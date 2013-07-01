#ifndef __SIFT_ASSERT_H
#define __SIFT_ASSERT_H

#include <cassert>

// Define our own version of assert() with a handler that can be overridden through weak linkage

extern void __sift_assert_fail(__const char *__assertion, __const char *__file,
                               unsigned int __line, __const char *__function)
       __THROW __attribute__ ((__noreturn__));

# define sift_assert(expr)                                                   \
  ((expr)                                                               \
   ? __ASSERT_VOID_CAST (0)                                             \
   : __sift_assert_fail (__STRING(expr), __FILE__, __LINE__, __ASSERT_FUNCTION))

#endif // __SIFT_ASSERT_H
