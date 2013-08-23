#ifndef __ROUTINE_TRACER_ONDEMAND_H
#define __ROUTINE_TRACER_ONDEMAND_H

#include "routine_tracer.h"

#include <unordered_map>

class RoutineTracerOndemand
{
   public:
      class RtnMaster : public RoutineTracer
      {
         public:
            RtnMaster();
            virtual ~RtnMaster() {}

            virtual RoutineTracerThread* getThreadHandler(Thread *thread) { return new RtnThread(this, thread); }
            virtual void addRoutine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename);
            virtual bool hasRoutine(IntPtr eip);
            RoutineTracer::Routine* getRoutine(IntPtr eip);

         private:
            static void signalHandler(int);

            Lock m_lock;
            std::unordered_map<IntPtr, RoutineTracer::Routine*> m_routines;
      };

      class RtnThread : public RoutineTracerThread
      {
         public:
            RtnThread(RtnMaster *master, Thread *thread) : RoutineTracerThread(thread), m_master(master) {}

            void printStack();

         private:
            RtnMaster *m_master;

         protected:
            virtual void functionEnter(IntPtr eip) {}
            virtual void functionExit(IntPtr eip) {}
            virtual void functionChildEnter(IntPtr eip, IntPtr eip_child) {}
            virtual void functionChildExit(IntPtr eip, IntPtr eip_child) {}
      };
};

#endif // __ROUTINE_TRACER_ONDEMAND_H
