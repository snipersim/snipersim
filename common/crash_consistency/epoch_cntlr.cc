#include "epoch_cntlr.h"

VersionedDomain::VersionedDomain(const UInt32 id) : m_id(id)
{
   // registerStatsMetric("epoch", m_id, "system_eid", &m_eid);
}

VersionedDomain::~VersionedDomain() = default;

EpochCntlr::EpochCntlr(const UInt32 vd_id, std::vector<core_id_t> cores) : m_vd(vd_id), m_cores(cores)
{
}

EpochCntlr::~EpochCntlr()
{
}

UInt64 EpochCntlr::getInstructionCount()
{
   UInt64 count = 0;
   for(core_id_t core_id : m_cores)
      count += Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel()->getInstructionCount();
   return count;
}