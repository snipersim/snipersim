#include "hooks_manager.h"
#include "log.h"

const char* HookType::hook_type_names[] = {
   "HOOK_PERIODIC",
   "HOOK_PERIODIC_INS",
   "HOOK_SIM_START",
   "HOOK_SIM_END",
   "HOOK_ROI_BEGIN",
   "HOOK_ROI_END",
   "HOOK_CPUFREQ_CHANGE",
   "HOOK_MAGIC_MARKER",
   "HOOK_MAGIC_USER",
   "HOOK_INSTR_COUNT",
   "HOOK_THREAD_START",
   "HOOK_THREAD_EXIT",
   "HOOK_THREAD_STALL",
   "HOOK_THREAD_RESUME",
   "HOOK_THREAD_MIGRATE",
   "HOOK_INSTRUMENT_MODE",
   "HOOK_PRE_STAT_WRITE",
   "HOOK_SYSCALL_ENTER",
   "HOOK_SYSCALL_EXIT",
};
static_assert(HookType::HOOK_TYPES_MAX == sizeof(HookType::hook_type_names) / sizeof(HookType::hook_type_names[0]),
              "Not enough values in HookType::hook_type_names");

HooksManager::HooksManager()
{
}

void HooksManager::registerHook(HookType::hook_type_t type, HookCallbackFunc func, UInt64 argument)
{
   m_registry[type].push_back(std::pair<HookCallbackFunc, UInt64>(func, argument));
}

SInt64 HooksManager::callHooks(HookType::hook_type_t type, UInt64 arg, bool expect_return)
{
   for(std::vector<HookCallback>::iterator it = m_registry[type].begin(); it != m_registry[type].end(); ++it)
   {
      SInt64 result = (it->first)(it->second, arg);
      if (expect_return && result != -1)
         return result;
   }

   return -1;
}
