#include "epoch_cntlr.h"
#include "epoch_manager.h"

VersionedDomain::VersionedDomain(const UInt32 id) : m_id(id)
{
   // registerStatsMetric("epoch", m_id, "system_eid", &m_eid);
}

VersionedDomain::~VersionedDomain() = default;

EpochCntlr::EpochCntlr(EpochManager* epoch_manager, const UInt32 vd_id, std::vector<core_id_t> cores) : 
                       m_epoch_manager(epoch_manager), m_vd(vd_id), m_cores(cores) { }

EpochCntlr::~EpochCntlr() = default;

void EpochCntlr::newEpoch()
{
   m_vd.increment();
}

void EpochCntlr::commit()
{
   m_epoch_manager->commit();
}

void EpochCntlr::registerPersistedEID(UInt64 persisted_eid)
{
   m_epoch_manager->registerPersistedEID(persisted_eid);
}

UInt64 EpochCntlr::getInstructionCount()
{
   UInt64 count = 0;
   for(core_id_t core_id : m_cores)
      count += Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel()->getInstructionCount();
   return count;
}
