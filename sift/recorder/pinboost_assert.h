#ifndef __PINBOOST_ASSERT_H
#define __PINBOOST_ASSERT_H

void pinboost_assert_fail(const char *__assertion, const char *__file,
                          unsigned int __line, const char *__function)
     __attribute__ ((__noreturn__));

#define __PINBOOST_ASSERT_VOID_CAST (void)

#if defined __cplusplus
#   define __ASSERT_FUNCTION    __PRETTY_FUNCTION__
# else
#   define __ASSERT_FUNCTION    __func__
#endif

# define pinboost_assert(expr)                                                   \
  ((expr)                                                                        \
   ? __PINBOOST_ASSERT_VOID_CAST (0)                                             \
   : pinboost_assert_fail (__STRING(expr), __FILE__, __LINE__, __ASSERT_FUNCTION))

#endif // __PINBOOST_ASSERT_H
