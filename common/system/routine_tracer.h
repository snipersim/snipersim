#ifndef __ROUTINE_TRACER_H
#define __ROUTINE_TRACER_H

#include "fixed_types.h"
#include "subsecond_time.h"

#include <deque>

class Thread;

class RoutineTracerThread
{
   public:
      RoutineTracerThread(Thread *thread);
      virtual ~RoutineTracerThread();

      void routineEnter(IntPtr eip, IntPtr esp);
      void routineExit(IntPtr eip, IntPtr esp);
      void routineAssert(IntPtr eip, IntPtr esp);

   protected:
      Lock m_lock;
      Thread *m_thread;
      std::deque<IntPtr> m_stack;
      IntPtr m_last_esp;

   private:
      bool unwindTo(IntPtr eip);

      void routineEnter_unlocked(IntPtr eip, IntPtr esp);

      virtual void functionEnter(IntPtr eip) {}
      virtual void functionExit(IntPtr eip) {}
      virtual void functionChildEnter(IntPtr eip, IntPtr eip_child) {}
      virtual void functionChildExit(IntPtr eip, IntPtr eip_child) {}

      void hookRoiBegin();
      void hookRoiEnd();
      static SInt64 __hook_roi_begin(UInt64 user, UInt64 arg) { ((RoutineTracerThread*)user)->hookRoiBegin(); return 0; }
      static SInt64 __hook_roi_end(UInt64 user, UInt64 arg) { ((RoutineTracerThread*)user)->hookRoiEnd(); return 0; }
};

class RoutineTracer
{
   public:
      class Routine
      {
         public:
            const IntPtr m_eip;
            char *m_name;
            char *m_location;

            Routine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename);
            void updateLocation(const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename);
      };

      static RoutineTracer* create();

      RoutineTracer();
      virtual ~RoutineTracer();

      virtual void addRoutine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename) = 0;
      virtual bool hasRoutine(IntPtr eip) = 0;
      virtual RoutineTracerThread* getThreadHandler(Thread *thread) = 0;
};

#endif // __ROUTINE_TRACER_H
