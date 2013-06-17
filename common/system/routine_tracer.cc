#include "routine_tracer.h"
#include "simulator.h"
#include "magic_server.h"
#include "config.hpp"
#include "routine_tracer_print.h"
#include "routine_tracer_funcstats.h"

#include <cstring>

RoutineTracerThread::RoutineTracerThread(Thread *thread)
   : m_thread(thread)
{
   Sim()->getHooksManager()->registerHook(HookType::HOOK_ROI_BEGIN, __hook_roi_begin, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_ROI_END, __hook_roi_end, (UInt64)this);
}

RoutineTracerThread::~RoutineTracerThread()
{
}

void RoutineTracerThread::routineEnter(IntPtr eip, IntPtr esp)
{
   if (m_stack.size())
      if (Sim()->getMagicServer()->inROI())
         functionChildEnter(m_stack.back(), eip);

   m_stack.push_back(eip);
   m_last_esp = esp;

   if (Sim()->getMagicServer()->inROI())
      functionEnter(eip);
}

void RoutineTracerThread::routineExit(IntPtr eip, IntPtr esp)
{
   if (m_stack.size() == 0)
      return;

   bool found = true;
   if (m_stack.back() != eip)
   {
      // If we are returning from a function that's not at the top of the stack, search for it further down
      found = unwindTo(eip);
   }

   if (!found)
   {
      // Mismatch, ignore
   }
   else
   {
      // Unwound into eip, now exit it
      if (Sim()->getMagicServer()->inROI())
         functionExit(eip);
      m_stack.pop_back();
   }

   m_last_esp = esp;

   if (m_stack.size())
      if (Sim()->getMagicServer()->inROI())
         functionChildExit(m_stack.back(), eip);
}

void RoutineTracerThread::routineAssert(IntPtr eip, IntPtr esp)
{
   if (m_stack.size() == 0)
   {
      // Newly created thread just jumps into the first routine
      routineEnter(eip, esp);
   }
   else if (m_stack.back() == eip)
   {
      // We are where we think we are, no action
   }
   else if (esp <= m_last_esp)
   {
      // Stack grew (downwards), or stayed constant (tail call): we entered a new function
      routineEnter(eip, esp);
   }
   else
   {
      bool found = unwindTo(eip);

      if (!found)
      {
         // We now seem to be in a function we haven't been before, enter it (tail call elimination?)
         routineEnter(eip, esp);
      }
      else
      {
         // Jumped back into a function further down the stack (longjmp?)
         // unwindTo has already popped the stack
         m_last_esp = esp;
      }

      // After all this, the current function should be at the top of the stack
      LOG_ASSERT_ERROR(m_stack.back() == eip, "Expected to be in function %lx but now in %lx", eip, m_stack.back());
   }
}

bool RoutineTracerThread::unwindTo(IntPtr eip)
{
   for(std::deque<IntPtr>::reverse_iterator it = m_stack.rbegin(); it != m_stack.rend(); ++it)
   {
      if (*it == eip)
      {
         // We found this eip further down the stack: unwind
         while(m_stack.back() != eip)
         {
            if (Sim()->getMagicServer()->inROI())
               functionExit(m_stack.back());
            m_stack.pop_back();
            if (Sim()->getMagicServer()->inROI())
               functionChildExit(m_stack.back(), eip);
         }
         return true;
      }
   }
   return false;
}

void RoutineTracerThread::hookRoiBegin()
{
   for(std::deque<IntPtr>::iterator it = m_stack.begin(); it != m_stack.end(); ++it)
      functionEnter(*it);
}

void RoutineTracerThread::hookRoiEnd()
{
   for(std::deque<IntPtr>::reverse_iterator it = m_stack.rbegin(); it != m_stack.rend(); ++it)
      functionExit(*it);
}

RoutineTracer::Routine::Routine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename)
   : m_eip(eip)
   , m_name(NULL)
   , m_location(NULL)
{
   updateLocation(name, imgname, offset, column, line, filename);
}

void RoutineTracer::Routine::updateLocation(const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename)
{
   if (m_name)
      free(m_name);
   if (m_location)
      free(m_location);

   m_name = strdup(name);
   char location[4096];
   snprintf(location, 4095, "%s:%" PRIdPTR ":%s:%d:%d", imgname, offset, filename, line, column);
   location[4095] = '\0';
   m_location = strdup(location);
}

RoutineTracer::RoutineTracer()
{
}

RoutineTracer::~RoutineTracer()
{
}

RoutineTracer* RoutineTracer::create()
{
   String type = Sim()->getCfg()->getString("routine_tracer/type");

   if (type == "none")
      return NULL;
   else if (type == "print")
      return new RoutineTracerPrint::RtnMaster();
   else if (type == "funcstats")
      return new RoutineTracerFunctionStats::RtnMaster();
   else
      LOG_PRINT_ERROR("Unknown routine tracer type %s", type.c_str());
}
