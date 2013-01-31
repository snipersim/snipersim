#ifndef IOCOOM_PERFORMANCE_MODEL_H
#define IOCOOM_PERFORMANCE_MODEL_H

#include <vector>

#include "performance_model.h"
#include "subsecond_time.h"

/*
  In-order core, out-of-order memory performance model.

  We use a simpe scoreboard to keep track of registers.

  We also keep a store buffer to short circuit loads.
 */
class IOCOOMPerformanceModel : public PerformanceModel
{
public:
   IOCOOMPerformanceModel(Core* core);
   ~IOCOOMPerformanceModel();

private:

   bool handleInstruction(Instruction const* instruction);

   void modelIcache(IntPtr address, UInt32 size);
   std::pair<const SubsecondTime,const SubsecondTime> executeLoad(SubsecondTime time, const DynamicInstructionInfo &);
   SubsecondTime executeStore(SubsecondTime time, const DynamicInstructionInfo &);

   typedef std::vector<SubsecondTime> Scoreboard;

   class LoadUnit
   {
   public:
      LoadUnit(unsigned int num_units);
      ~LoadUnit();

      SubsecondTime execute(SubsecondTime time, SubsecondTime occupancy);

   private:
      Scoreboard m_scoreboard;
   };

   class StoreBuffer
   {
   public:
      enum Status
      {
         VALID,
         COMPLETED,
         NOT_FOUND
      };

      StoreBuffer(unsigned int num_entries);
      ~StoreBuffer();

      /*
        @return Time store finishes.
        @param time Time store starts.
        @param addr Address of store.
      */
      SubsecondTime executeStore(SubsecondTime time, SubsecondTime occupancy, IntPtr addr);

      /*
        @return True if addr is in store buffer at given time.
        @param time Time to check for addr.
        @param addr Address to check.
      */
      Status isAddressAvailable(SubsecondTime time, IntPtr addr);

   private:
      Scoreboard m_scoreboard;
      std::vector<IntPtr> m_addresses;
   };

   Scoreboard m_register_scoreboard;
   StoreBuffer *m_store_buffer;
   LoadUnit *m_load_unit;
};

#endif // IOCOOM_PERFORMANCE_MODEL_H
