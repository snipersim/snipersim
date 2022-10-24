#include "epoch_manager.h"

EpochManager::EpochManager()  : m_log_file(nullptr)
                              , m_system_eid(0)
                              , m_max_interval_time(SubsecondTime::NS(getMaxIntervalTime()))
                              , m_max_interval_instr(getMaxIntervalInstructions())
{
   bzero(&m_commited, sizeof(m_commited));
   bzero(&m_persisted, sizeof(m_persisted));

   Sim()->getHooksManager()->registerHook(HookType::HOOK_APPLICATION_START, _start, (UInt64) this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_APPLICATION_EXIT, _exit, (UInt64) this);
   
   // --- Only for test! ---
   Sim()->getHooksManager()->registerHook(HookType::HOOK_EPOCH_TIMEOUT, _commit, (UInt64) this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_EPOCH_TIMEOUT_INS, _commit, (UInt64) this);

   registerStatsMetric("epoch", 0, "system_eid", &m_system_eid);
   registerStatsMetric("epoch", 0, "commited_eid", &m_commited.eid);
   registerStatsMetric("epoch", 0, "persisted_eid", &m_persisted.eid);
}

EpochManager::~EpochManager() = default;

void EpochManager::start()
{
   const String filename = "sim.ckpts.csv";
   const String path = Sim()->getConfig()->getOutputDirectory() + "/" + filename.c_str();

   if ((m_log_file = fopen(path.c_str(), "w")) == nullptr)
   {
      fprintf(stderr, "Error on creating %s\n", filename.c_str());
      assert(m_log_file != nullptr);
   }   

   if (m_max_interval_time.getNS() > 0)
      Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC, _interrupt, (UInt64) this);
   if (m_max_interval_instr > 0)
      Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC_INS, _interrupt, (UInt64) this);

   Sim()->getHooksManager()->callHooks(HookType::HOOK_EPOCH_START, ++m_system_eid);
}

void EpochManager::exit()
{
   Sim()->getHooksManager()->callHooks(HookType::HOOK_EPOCH_END, m_system_eid);

   fclose(m_log_file);
}

void EpochManager::interrupt()
{
   if (m_max_interval_time.getNS() > 0)
   {
      auto now = Sim()->getClockSkewMinimizationServer()->getGlobalTime();
      auto gap = gapBetweenCheckpoints<SubsecondTime>(now, getCommitedTime());
      if (gap >= m_max_interval_time)
         Sim()->getHooksManager()->callHooks(HookType::HOOK_EPOCH_TIMEOUT, m_system_eid);
   }
   if (m_max_interval_instr > 0)
   {
      auto gap = gapBetweenCheckpoints<UInt64>(getInstructionCount(), getCommitedInstruction());
      if (gap >= m_max_interval_instr)
         Sim()->getHooksManager()->callHooks(HookType::HOOK_EPOCH_TIMEOUT_INS, m_system_eid);
   }
}

void EpochManager::commit()
{
   Sim()->getHooksManager()->callHooks(HookType::HOOK_EPOCH_END, m_system_eid);

   m_commited.eid = m_system_eid;
   m_commited.instr = getInstructionCount();
   m_commited.time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();

   printf("Commited Epoch (%lu)\n- Inst: %lu\n- Time: %lu\n", m_commited.eid, m_commited.instr, m_commited.time.getNS());
   fprintf(m_log_file, "%lu\n", m_commited.time.getNS());
   
   Sim()->getHooksManager()->callHooks(HookType::HOOK_EPOCH_START, ++m_system_eid);
}

void EpochManager::registerPersistedEID(UInt64 eid)
{
   m_persisted.eid = eid;

   Sim()->getHooksManager()->callHooks(HookType::HOOK_EPOCH_PERSISTED, eid);
}

UInt64 EpochManager::getInstructionCount()
{
   UInt64 count = 0;
   for(core_id_t core_id = 0; core_id < (core_id_t) Sim()->getConfig()->getApplicationCores(); core_id++)
      count += Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel()->getInstructionCount();
   return count;
}

template <typename T>
T EpochManager::gapBetweenCheckpoints(T current, T last) {
   return current >= last ? current - last : last - current;
}

UInt64 EpochManager::getMaxIntervalTime()
{
   const String key = "epoch/max_interval_time";

   SInt64 max_interval_time = Sim()->getCfg()->hasKey(key) ? Sim()->getCfg()->getInt(key) : 0;
   assert(max_interval_time >= 0);

   if (max_interval_time != 0)
      assert(max_interval_time >= 1000); // (1000ns == 1μs)

   return (UInt64) max_interval_time;
}

UInt64 EpochManager::getMaxIntervalInstructions()
{
   const String key = "epoch/max_interval_instructions";
   
   SInt64 max_interval_instructions = Sim()->getCfg()->hasKey(key) ? Sim()->getCfg()->getInt(key) : 0;
   assert(max_interval_instructions >= 0);

   if (max_interval_instructions != 0)
   {
      SInt64 ins_per_core = Sim()->getCfg()->getInt("core/hook_periodic_ins/ins_per_core");
      SInt64 ins_global = Sim()->getCfg()->getInt("core/hook_periodic_ins/ins_global");
      assert(max_interval_instructions >= ins_global);
      assert((ins_global >= ins_per_core) && (max_interval_instructions % ins_global == 0));
   }

   return (UInt64) max_interval_instructions;
}

EpochManager *EpochManager::getInstance()
{
   return Sim()->getEpochManager();
}

UInt64 EpochManager::getGlobalSystemEID()
{
   return Sim()->getEpochManager()->getSystemEID();
}
