#ifndef __HOOKS_MANAGER_H
#define __HOOKS_MANAGER_H

#include "fixed_types.h"
#include "subsecond_time.h"

#include <vector>
#include <unordered_map>

class HookType
{
public:
   enum hook_type_t {
      // Hook name              Parameter (cast from void*)          Description
      HOOK_PERIODIC,            // SubsecondTime current_time        Barrier was reached
      HOOK_SIM_START,           // none                              Simulation start
      HOOK_SIM_END,             // none                              Simulation end
      HOOK_ROI_BEGIN,           // none                              ROI begin
      HOOK_ROI_END,             // none                              ROI end
      HOOK_CPUFREQ_CHANGE,      // UInt64 coreid                     CPU frequency was changed
      HOOK_MAGIC_MARKER,        // MagicServer::MagicMarkerType *    Magic marker (SimMarker) in application
      HOOK_MAGIC_USER,          // MagicServer::MagicMarkerType *    Magic user function (SimUser) in application
      HOOK_INSTR_COUNT,         // UInt64 coreid                     Core has executed a preset number of instructions
      HOOK_THREAD_STALL,        // HooksManager::ThreadStall         Core has entered stalled state
      HOOK_THREAD_RESUME,       // HooksManager::ThreadResume        Core has entered running state
      HOOK_INSTRUMENT_MODE,     // UInt64 Instrument Mode            Simulation mode change (ex. detailed, ffwd)
      HOOK_TYPES_MAX
   };
   static const char* hook_type_names[];
};

namespace std
{
   template <> struct hash<HookType::hook_type_t> {
      size_t operator()(const HookType::hook_type_t & type) const {
         //return std::hash<int>(type);
         return (int)type;
      }
   };
}

class HooksManager
{
public:
   typedef SInt64 (*HookCallbackFunc)(void*, void*);
   typedef std::pair<HookCallbackFunc, void*> HookCallback;

   typedef struct {
      core_id_t core_id;      // Core stalling
      subsecond_time_t time;  // Time at which the stall occurs (if known, else SubsecondTime::MaxTime())
   } ThreadStall;
   typedef struct {
      core_id_t core_id;      // Core being woken up
      core_id_t core_by;      // Core triggering the wakeup
      subsecond_time_t time;  // Time at which the wakeup occurs (if known, else SubsecondTime::MaxTime())
   } ThreadResume;

   HooksManager();
   void init();
   void fini();
   void registerHook(HookType::hook_type_t type, HookCallbackFunc func, void* argument);
   SInt64 callHooks(HookType::hook_type_t type, void* argument, bool expect_return = false);

private:
   std::unordered_map<HookType::hook_type_t, std::vector<HookCallback> > m_registry;
};

#endif /* __HOOKS_MANAGER_H */
