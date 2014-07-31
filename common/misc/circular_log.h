#ifndef __CIRCULAR_LOG_H
#define __CIRCULAR_LOG_H

#include "fixed_types.h"
#include "lock.h"
#include "timer.h"

// Fast circular log to trace generic events
//
// Events consist of a printf-style format string and up to 6 arguments.
// To reduce overhead, printf itself is not called during logging;
// instead the format string and all arguments are stored in a circular buffer.
// On simulation end or abort, the last 1M entries are formatted and written to sim.clog.
// This means the arguments must remain valid until simulation end, which is fine
// for integers and const char*. Pointers into dynamically allocated memory will be
// invalid by the time they are dereferenced and should thus not be used.

class CircularLog
{
   public:
      static void init(String filename);
      static void enableCallbacks();
      static void fini();
      static void dump();

      static CircularLog *g_singleton;

      CircularLog(String filename);
      ~CircularLog();

      void insert(const char* type, const char* msg, ...) __attribute__ ((format(printf, 3, 4)));
      UInt64 getTime() const { return rdtsc() - m_time_zero; }

   private:
      typedef struct {
         UInt64 time;
         const char* type;
         const char* msg;
         UInt64 args[6];
      } event_t;

      static SInt64 hook_sigusr1(UInt64, UInt64) { dump(); return 0; }

      void writeLog();
      void writeEntry(FILE *fp, int idx);

      static const UInt64 BUFFER_SIZE = 1024*1024;

      const String m_filename;
      event_t* const m_buffer;
      Lock m_lock;
      UInt64 m_eventnum;
      UInt64 m_time_zero;
};

#define CLOG(type, ...) do { \
      if (CircularLog::g_singleton) \
         CircularLog::g_singleton->insert(type, __VA_ARGS__); \
   } while(0)

#endif // __CIRCULAR_LOG_H
