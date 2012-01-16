#ifndef __CLOCK_SKEW_MINIMIZATION_OBJECT_H__
#define __CLOCK_SKEW_MINIMIZATION_OBJECT_H__

// Forward Decls
class Core;
class Network;
class UnstructuredBuffer;
class NetPacket;

#include "fixed_types.h"
#include "subsecond_time.h"

class ClockSkewMinimizationObject
{
   public:
      enum Scheme
      {
         NONE = 0,
         BARRIER,
         RING,
         RANDOM_PAIRS,
         FINE_GRAINED,
         NUM_SCHEMES
      };

      static Scheme parseScheme(String scheme);
};

void ClockSkewMinimizationClientNetworkCallback(void* obj, NetPacket packet);

class ClockSkewMinimizationClient : public ClockSkewMinimizationObject
{
protected:
   ClockSkewMinimizationClient() {}

public:
   ~ClockSkewMinimizationClient() {}
   static ClockSkewMinimizationClient* create(String scheme_str, Core* core);

   virtual void enable() = 0;
   virtual void disable() = 0;
   virtual void synchronize(SubsecondTime time = SubsecondTime::Zero(), bool ignore_time = false, bool abort_func(void*) = NULL, void* abort_arg = NULL) = 0;
   virtual void netProcessSyncMsg(const NetPacket& recv_pkt) = 0;
};

class ClockSkewMinimizationManager : public ClockSkewMinimizationObject
{
protected:
   ClockSkewMinimizationManager() {}

public:
   ~ClockSkewMinimizationManager() {}
   static ClockSkewMinimizationManager* create(String scheme_str);

   virtual void processSyncMsg(Byte* msg) = 0;
};

class ClockSkewMinimizationServer : public ClockSkewMinimizationObject
{
protected:
   ClockSkewMinimizationServer() {}

public:
   ~ClockSkewMinimizationServer() {}
   static ClockSkewMinimizationServer* create(String scheme_str, Network& network, UnstructuredBuffer& recv_buff);

   virtual void processSyncMsg(core_id_t core_id) = 0;
   virtual void signal() = 0;
   virtual void setFastForward(bool fastforward, SubsecondTime next_barrier_time = SubsecondTime::MaxTime()) = 0;
   virtual SubsecondTime getGlobalTime();
};

#endif /* __CLOCK_SKEW_MINIMIZATION_OBJECT_H__ */
