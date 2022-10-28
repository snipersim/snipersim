#ifndef EPOCH_CNTLR_H
#define EPOCH_CNTLR_H

#include "simulator.h"
#include "core_manager.h"
#include "performance_model.h"
#include "stats.h"

class VersionedDomain
{
public:

   VersionedDomain(const UInt32 id);
   ~VersionedDomain();

   void increment() { m_eid++; }

   void synchronize(UInt64 remote_eid);

   UInt32 getID() const { return m_id; }
   UInt64 getEpochID() const { return m_eid; }

private:

   const UInt32 m_id;
   UInt64 m_eid;
};

class EpochCntlr
{
public:

   /**
    * @brief Construct a new Epoch Cntlr
    */
   EpochCntlr(const UInt32 vd_id, std::vector<core_id_t> cores);

   /**
    * @brief Destroy the Epoch Cntlr
    */
   ~EpochCntlr();

   UInt64 getInstructionCount();

   UInt64 getCurrentEID() const { return m_vd.getEpochID(); }

   const VersionedDomain* getVersionedDomain() const { return &m_vd; }

private:

   VersionedDomain m_vd;
   std::vector<core_id_t> m_cores;
};

#endif /* EPOCH_CNTLR_H */
