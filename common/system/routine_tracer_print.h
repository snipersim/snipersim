#ifndef __ROUTINE_TRACER_PRINT_H
#define __ROUTINE_TRACER_PRINT_H

#include "routine_tracer.h"

#include <unordered_map>

class RoutineTracerPrint
{
   public:
      class RtnMaster : public RoutineTracer
      {
         public:
            RtnMaster() {}
            virtual ~RtnMaster() {}

            virtual RoutineTracerThread* getThreadHandler(Thread *thread);
            virtual void addRoutine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename);
            virtual bool hasRoutine(IntPtr eip);
            RoutineTracer::Routine* getRoutine(IntPtr eip);

         private:
            Lock m_lock;
            std::unordered_map<IntPtr, RoutineTracer::Routine*> m_routines;
      };

      class RtnThread : public RoutineTracerThread
      {
         public:
            RtnThread(RtnMaster *master, Thread *thread);

         private:
            RtnMaster *m_master;
            UInt64 m_depth;

         protected:
            virtual void functionEnter(IntPtr eip);
            virtual void functionExit(IntPtr eip);
            virtual void functionChildEnter(IntPtr eip, IntPtr eip_child) {}
            virtual void functionChildExit(IntPtr eip, IntPtr eip_child) {}
      };
};

#endif // __ROUTINE_TRACER_PRINT_H
