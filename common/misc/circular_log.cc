#include "circular_log.h"
#include "log.h"

#include <stdarg.h>

CircularLog* CircularLog::g_singleton = NULL;

void CircularLog::init(String filename)
{
   g_singleton = new CircularLog(filename);
}

void CircularLog::fini()
{
   if (g_singleton)
   {
      delete g_singleton;
      g_singleton = NULL;
   }
}

CircularLog::CircularLog(String filename)
   : m_filename(filename)
   , m_buffer(new event_t[BUFFER_SIZE])
   , m_eventnum(0)
   , m_time_zero(rdtsc())
{
}

CircularLog::~CircularLog()
{
   FILE *fp = fopen(m_filename.c_str(), "w");

   UInt64 head = m_eventnum % BUFFER_SIZE;
   if (head != m_eventnum)
   {
      fprintf(fp, "... %ld prior events ...\n", m_eventnum - BUFFER_SIZE);
      for(UInt64 idx = head; idx < BUFFER_SIZE; ++idx)
         writeEntry(fp, idx);
   }
   for(UInt64 idx = 0; idx < head; ++idx)
      writeEntry(fp, idx);

   fclose(fp);
   delete m_buffer;
}

void CircularLog::insert(const char* type, const char* msg, ...)
{
   int position = __sync_fetch_and_add(&m_eventnum, 1) % BUFFER_SIZE;

   m_buffer[position].time  = getTime();
   m_buffer[position].type  = type;
   m_buffer[position].msg   = msg;

   va_list args;
   va_start(args, msg);
   for(int i = 0; i < 6; ++i)
      m_buffer[position].args[i] = va_arg(args, UInt64);
   va_end(args);
}

void CircularLog::writeEntry(FILE *fp, int idx)
{
   event_t &e = m_buffer[idx];
   fprintf(fp, "%12" PRId64 " [%s] ", e.time, e.type);
   fprintf(fp, e.msg, e.args[0], e.args[1], e.args[2], e.args[3], e.args[4], e.args[5]);
   fprintf(fp, "\n");
}
