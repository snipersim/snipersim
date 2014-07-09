#include "cheetah_manager.h"
#include "cheetah_model.h"
#include "simulator.h"
#include "config.hpp"
#include "core_manager.h"
#include "hooks_manager.h"
#include "stats.h"

CheetahManager::CheetahStats *CheetahManager::s_cheetah_stats = NULL;
std::vector<std::vector<CheetahModel*> > CheetahManager::s_cheetah_models(NUM_CHEETAH_TYPES);
const char* CheetahManager::cheetah_names[] = { "local", "by-2", "by-4", "by-8", "global" };

CheetahManager::CheetahManager(core_id_t core_id)
   : m_min_bits(Sim()->getCfg()->getInt("core/cheetah/min_size_bits"))
   , m_max_bits_local(Sim()->getCfg()->getInt("core/cheetah/max_size_bits_local"))
   , m_max_bits_global(Sim()->getCfg()->getInt("core/cheetah/max_size_bits_global"))
   , m_address_buffer_size(0)
{
   LOG_ASSERT_ERROR(m_min_bits >= CheetahModel::getMinSize(),
      "cheetah/min_size_bits (%d) must be >= %d",
      m_min_bits, CheetahModel::getMinSize());
   LOG_ASSERT_ERROR(m_max_bits_local >= CheetahModel::getMinSize(),
      "cheetah/max_size_bits_local (%d) must be >= %d",
      m_max_bits_local, CheetahModel::getMinSize());
   LOG_ASSERT_ERROR(m_max_bits_global >= CheetahModel::getMinSize(),
      "cheetah/max_size_bits_global (%d) must be >= %d",
      m_max_bits_global, CheetahModel::getMinSize());

   if (!s_cheetah_stats)
      s_cheetah_stats = new CheetahStats(m_min_bits, m_max_bits_local, m_max_bits_global);

   s_cheetah_models[CHEETAH_LOCAL].push_back(new CheetahModel(false, m_min_bits, m_max_bits_local));
   if ((core_id & 1) == 0) s_cheetah_models[CHEETAH_BY2].push_back(new CheetahModel(true, m_min_bits, m_max_bits_local));
   if ((core_id & 3) == 0) s_cheetah_models[CHEETAH_BY4].push_back(new CheetahModel(true, m_min_bits, m_max_bits_local));
   if ((core_id & 7) == 0) s_cheetah_models[CHEETAH_BY8].push_back(new CheetahModel(true, m_min_bits, m_max_bits_local));
   if (core_id == 0)       s_cheetah_models[CHEETAH_GLOBAL].push_back(new CheetahModel(true, m_min_bits, m_max_bits_global));

   m_cheetah[CHEETAH_LOCAL] = s_cheetah_models[CHEETAH_LOCAL].back();
   m_cheetah[CHEETAH_BY2] = s_cheetah_models[CHEETAH_BY2].back();
   m_cheetah[CHEETAH_BY4] = s_cheetah_models[CHEETAH_BY4].back();
   m_cheetah[CHEETAH_BY8] = s_cheetah_models[CHEETAH_BY8].back();
   m_cheetah[CHEETAH_GLOBAL] = s_cheetah_models[CHEETAH_GLOBAL].back();
}

CheetahManager::~CheetahManager()
{
}

void CheetahManager::access(Core::mem_op_t mem_op_type, IntPtr address)
{
   m_address_buffer[m_address_buffer_size++] = address;

   if (m_address_buffer_size >= ADDRESS_BUFFER_SIZE)
   {
      for(unsigned int idx = 0; idx < NUM_CHEETAH_TYPES; ++idx)
         m_cheetah[idx]->accesses(m_address_buffer, m_address_buffer_size);
      m_address_buffer_size = 0;
   }
}

CheetahManager::CheetahStats::CheetahStats(UInt32 min_bits, UInt32 max_bits_local, UInt32 max_bits_global)
   : m_min_bits(min_bits)
   , m_max_bits_local(max_bits_local)
   , m_max_bits_global(max_bits_global)
{
   m_stats.resize(NUM_CHEETAH_TYPES);
   for(unsigned int idx = 0; idx < NUM_CHEETAH_TYPES; ++idx)
   {
      UInt32 max_bits = (idx == CHEETAH_GLOBAL ? max_bits_global : max_bits_local);
      m_stats[idx].resize(max_bits + 1);
      for(UInt32 size = 0; size < max_bits; ++size)
         registerStatsMetric("cheetah", size, cheetah_names[idx], &m_stats[idx][size]);
   }
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PRE_STAT_WRITE, hook_update, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
}

void CheetahManager::CheetahStats::update()
{
   for(unsigned int idx = 0; idx < NUM_CHEETAH_TYPES; ++idx)
   {
      for(UInt32 size_bits = 0; size_bits < m_stats.size(); ++size_bits)
         m_stats[idx][size_bits] = 0;
      for(auto it = s_cheetah_models[idx].begin(); it != s_cheetah_models[idx].end(); ++it)
         (*it)->updateStats(m_stats[idx]);
   }
}
