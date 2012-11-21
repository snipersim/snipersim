#ifndef LOG_H
#define LOG_H

#include "fixed_types.h"
#include "lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <map>

class Config;

class Log
{
   public:
      Log(Config &config);
      ~Log();

      static Log *getSingleton();

      enum ErrorState
      {
         None,
         Warning,
         Error,
      };

      void log(ErrorState err, const char *source_file, SInt32 source_line, const char* format, ...);

      bool isEnabled(const char* module);
      bool isLoggingEnabled() const { return _anyLoggingEnabled; }

      String getModule(const char *filename);

   private:
      UInt64 getTimestamp();

      void initFileDescriptors();
      static void parseModules(std::set<String> &mods, String list);
      void getDisabledModules();
      void getEnabledModules();
      bool initIsLoggingEnabled();

      void discoverCore(core_id_t *core_id, bool *sim_thread);
      void getFile(core_id_t core_id, bool sim_thread, FILE ** f, Lock ** l);

      ErrorState _state;

      // when core id is known
      FILE** _coreFiles;
      FILE** _simFiles;
      Lock* _coreLocks;
      Lock *_simLocks;

      // when core is id unknown but process # is
      FILE* _systemFile;
      Lock _systemLock;

      core_id_t _coreCount;
      UInt64 _startTime;
      std::set<String> _disabledModules;
      std::set<String> _enabledModules;
      bool _loggingEnabled;
      bool _anyLoggingEnabled;

      /* std::map<const char*, String> _modules; */
      /* Lock _modules_lock; */

      static const size_t MODULE_LENGTH = 20;

      static Log *_singleton;
};

// Macros

#ifdef NDEBUG

// see assert.h

#define __LOG_PRINT(...) ((void)(0))
#define _LOG_PRINT(...) ((void)(0))
#define LOG_PRINT(...) ((void)(0))
#define LOG_PRINT_WARNING(...) ((void)(0))
#define LOG_PRINT_WARNING_ONCE(...) ((void)(0))
#define LOG_PRINT_ERROR(...) (exit(-1)) // call exit() to inherit its __attribute__ ((__noreturn__))
#define LOG_ASSERT_WARNING(...) ((void)(0))
#define LOG_ASSERT_WARNING_ONCE(...) ((void)(0))
#define LOG_ASSERT_ERROR(...) ((void)(0))

#else

#define likely(x)       __builtin_expect((x), 1)
#define unlikely(x)     __builtin_expect((x), 0)

#define __LOG_PRINT(err, file, line, ...)                               \
   {                                                                    \
      if (unlikely(Log::getSingleton()->isLoggingEnabled()) || err != Log::None)  \
      {                                                                 \
         String module = Log::getSingleton()->getModule(file);     \
         if (err != Log::None ||                                        \
             Log::getSingleton()->isEnabled(module.c_str()))            \
         {                                                              \
            Log::getSingleton()->log(err, module.c_str(), line, __VA_ARGS__); \
         }                                                              \
      }                                                                 \
   }                                                                    \

#define _LOG_PRINT(err, ...)                                            \
   {                                                                    \
   __LOG_PRINT(err, __FILE__, __LINE__, __VA_ARGS__);                   \
   }                                                                    \

#define LOG_PRINT(...)                                                  \
   _LOG_PRINT(Log::None, __VA_ARGS__);                                  \

#define LOG_PRINT_WARNING(...)                  \
   _LOG_PRINT(Log::Warning, __VA_ARGS__);

#define LOG_PRINT_WARNING_ONCE(...)             \
   {                                            \
      static bool already_printed = false;      \
      if (!already_printed)                     \
      {                                         \
         _LOG_PRINT(Log::Warning, __VA_ARGS__); \
         _LOG_PRINT(Log::Warning, "Future warnings of this type will be suppressed."); \
         already_printed = true;                \
      }                                         \
   }

// _LOG_PRINT(Log::Error) does not return, but the compiler doesn't know this which can result in
// ''control reaches end of non-void function'' warnings in places where the LOG_PRINT_ASSERT(false) idiom is used.
// We cannot easily add __attribute__ ((__noreturn__)) to the LOG_PRINT_ERROR macro
// since that only works on functions, and we have the added complexity of passing the ... argument around.
// Adding exit() to the end of LOG_PRINT_ERROR effectively makes it inherit exit's __noreturn__
// which suppresses the warning (even though the exit() itself isn't executed as it's never reached).

#define LOG_PRINT_ERROR(...)                    \
   {                                            \
      _LOG_PRINT(Log::Error, __VA_ARGS__);      \
      exit(-1);                                 \
   }

#define LOG_ASSERT_WARNING(expr, ...)                   \
   {                                                    \
      if (!(expr))                                      \
      {                                                 \
         LOG_PRINT_WARNING(__VA_ARGS__);                \
      }                                                 \
   }                                                    \

#define LOG_ASSERT_WARNING_ONCE(expr, ...)              \
   {                                                    \
      if (!(expr))                                      \
      {                                                 \
         LOG_PRINT_WARNING_ONCE(__VA_ARGS__);           \
      }                                                 \
   }                                                    \

#define LOG_ASSERT_ERROR(expr, ...)                     \
   {                                                    \
      if (!(expr))                                      \
      {                                                 \
         LOG_PRINT_ERROR(__VA_ARGS__);                  \
      }                                                 \
   }                                                    \

#endif // NDEBUG

// Helpers

class FunctionTracer
{
public:
   FunctionTracer(const char *file, int line, const char *fn)
      : m_file(file)
      , m_line(line)
      , m_fn(fn)
   {
      __LOG_PRINT(Log::None, m_file, m_line, "Entering: %s", m_fn);
   }

   ~FunctionTracer()
   {
      __LOG_PRINT(Log::None, m_file, m_line, "Exiting: %s", m_fn);
   }

private:
   const char *m_file;
   int m_line;
   const char *m_fn;
};

#define LOG_FUNC_TRACE()   FunctionTracer func_tracer(__FILE__,__LINE__,__PRETTY_FUNCTION__);

#endif // LOG_H
