#ifndef __CLOCK_SKEW_MINIMIZATION_OBJECT_H__
#define __CLOCK_SKEW_MINIMIZATION_OBJECT_H__

#include "fixed_types.h"
#include "subsecond_time.h"
#include "log.h"

class Core;

class ClockSkewMinimizationObject
{
   public:
      enum Scheme
      {
         NONE = 0,
         BARRIER,
         NUM_SCHEMES
      };

      static Scheme parseScheme(String scheme);
};

class ClockSkewMinimizationClient : public ClockSkewMinimizationObject
{
protected:
   ClockSkewMinimizationClient() {}

public:
   virtual ~ClockSkewMinimizationClient() {}
   static ClockSkewMinimizationClient* create(Core* core);

   virtual void enable() = 0;
   virtual void disable() = 0;
   virtual void synchronize(SubsecondTime time = SubsecondTime::Zero(), bool ignore_time = false, bool abort_func(void*) = NULL, void* abort_arg = NULL) = 0;
};

class ClockSkewMinimizationManager : public ClockSkewMinimizationObject
{
protected:
   ClockSkewMinimizationManager() {}

public:
   virtual ~ClockSkewMinimizationManager() {}
   static ClockSkewMinimizationManager* create();

   virtual void processSyncMsg(Byte* msg) = 0;
};

class ClockSkewMinimizationServer : public ClockSkewMinimizationObject
{
protected:
   ClockSkewMinimizationServer() {}

public:
   virtual ~ClockSkewMinimizationServer() {}
   static ClockSkewMinimizationServer* create();

   virtual void synchronize(thread_id_t thread_id, SubsecondTime time) = 0;
   virtual void release() = 0;
   virtual void advance() = 0;
   virtual void setDisable(bool disable) { }
   virtual void setGroup(core_id_t core_id, core_id_t master_core_id) = 0;
   virtual void setFastForward(bool fastforward, SubsecondTime next_barrier_time = SubsecondTime::MaxTime()) = 0;
   virtual SubsecondTime getGlobalTime(bool upper_bound = false);
   virtual void setBarrierInterval(SubsecondTime barrier_interval) = 0;
   virtual SubsecondTime getBarrierInterval() const = 0;

   virtual void printState(void) {}
};

#endif /* __CLOCK_SKEW_MINIMIZATION_OBJECT_H__ */
