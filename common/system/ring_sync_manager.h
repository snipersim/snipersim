#ifndef __RING_SYNC_MANAGER_H__
#define __RING_SYNC_MANAGER_H__

#include <vector>

#include "clock_skew_minimization_object.h"
#include "transport.h"
#include "fixed_types.h"
#include "subsecond_time.h"

class RingSyncManager : public ClockSkewMinimizationManager
{
public:
   class CycleCountUpdate
   {
   public:
      SubsecondTime min_elapsed_time; // for next iteration
      SubsecondTime max_elapsed_time; // for current iteration
   };

   RingSyncManager();
   ~RingSyncManager();

   void processSyncMsg(Byte* msg);
   void generateSyncMsg(void);
   
private:
   std::vector<Core*> _core_list;
   Transport::Node *_transport;
   SubsecondTime _slack;

   void updateClientObjectsAndRingMsg(CycleCountUpdate* cycle_count_update);
};

#endif /* __RING_SYNC_MANAGER_H__ */
