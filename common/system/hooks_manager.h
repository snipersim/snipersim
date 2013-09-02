#ifndef __HOOKS_MANAGER_H
#define __HOOKS_MANAGER_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "thread_manager.h"

#include <vector>
#include <unordered_map>

class HookType
{
public:
   enum hook_type_t {
      // Hook name              Parameter (cast from UInt64)         Description
      HOOK_PERIODIC,            // SubsecondTime current_time        Barrier was reached
      HOOK_PERIODIC_INS,        // UInt64 icount                     Instruction-based periodic callback
      HOOK_SIM_START,           // none                              Simulation start
      HOOK_SIM_END,             // none                              Simulation end
      HOOK_ROI_BEGIN,           // none                              ROI begin
      HOOK_ROI_END,             // none                              ROI end
      HOOK_CPUFREQ_CHANGE,      // UInt64 coreid                     CPU frequency was changed
      HOOK_MAGIC_MARKER,        // MagicServer::MagicMarkerType *    Magic marker (SimMarker) in application
      HOOK_MAGIC_USER,          // MagicServer::MagicMarkerType *    Magic user function (SimUser) in application
      HOOK_INSTR_COUNT,         // UInt64 coreid                     Core has executed a preset number of instructions
      HOOK_THREAD_CREATE,       // HooksManager::ThreadCreate        Thread creation
      HOOK_THREAD_START,        // HooksManager::ThreadTime          Thread start
      HOOK_THREAD_EXIT,         // HooksManager::ThreadTime          Thread end
      HOOK_THREAD_STALL,        // HooksManager::ThreadStall         Thread has entered stalled state
      HOOK_THREAD_RESUME,       // HooksManager::ThreadResume        Thread has entered running state
      HOOK_THREAD_MIGRATE,      // HooksManager::ThreadMigrate       Thread was moved to a different core
      HOOK_INSTRUMENT_MODE,     // UInt64 Instrument Mode            Simulation mode change (ex. detailed, ffwd)
      HOOK_PRE_STAT_WRITE,      // none                              Before statistics are written (update generated stats now!)
      HOOK_SYSCALL_ENTER,       // SyscallMdl::HookSyscallEnter      Thread enters a system call
      HOOK_SYSCALL_EXIT,        // SyscallMdl::HookSyscallExit       Thread exist from system call
      HOOK_APPLICATION_START,   // app_id_t                          Application (re)start
      HOOK_APPLICATION_EXIT,    // app_id_t                          Application exit
      HOOK_APPLICATION_ROI_BEGIN, // none                            ROI begin, always triggers
      HOOK_APPLICATION_ROI_END,   // none                            ROI end, always triggers
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
   enum HookCallbackOrder {
      ORDER_NOTIFY_PRE,       // For callbacks that want to inspect state before any actions
      ORDER_ACTION,           // For callbacks that want to change simulator state based on the event
      ORDER_NOTIFY_POST,      // For callbacks that want to inspect state after any actions
      NUM_HOOK_ORDER,
   };

   typedef SInt64 (*HookCallbackFunc)(UInt64, UInt64);
   struct HookCallback {
      HookCallbackFunc func;
      UInt64 arg;
      HookCallbackOrder order;
      HookCallback(HookCallbackFunc _func, UInt64 _arg, HookCallbackOrder _order) : func(_func), arg(_arg), order(_order) {}
   };
   typedef struct {
      thread_id_t thread_id;
      thread_id_t creator_thread_id;
   } ThreadCreate;
   typedef struct {
      thread_id_t thread_id;
      subsecond_time_t time;
   } ThreadTime;
   typedef struct {
      thread_id_t thread_id;  // Thread stalling
      ThreadManager::stall_type_t reason; // Reason for thread stall
      subsecond_time_t time;  // Time at which the stall occurs (if known, else SubsecondTime::MaxTime())
   } ThreadStall;
   typedef struct {
      thread_id_t thread_id;  // Thread being woken up
      thread_id_t thread_by;  // Thread triggering the wakeup
      subsecond_time_t time;  // Time at which the wakeup occurs (if known, else SubsecondTime::MaxTime())
   } ThreadResume;
   typedef struct {
      thread_id_t thread_id;  // Thread being migrated
      core_id_t core_id;      // Core the thread is now running (or INVALID_CORE_ID == -1 for unscheduled)
      subsecond_time_t time;  // Current time
   } ThreadMigrate;

   HooksManager();
   void init();
   void fini();
   void registerHook(HookType::hook_type_t type, HookCallbackFunc func, UInt64 argument, HookCallbackOrder order = ORDER_NOTIFY_PRE);
   SInt64 callHooks(HookType::hook_type_t type, UInt64 argument, bool expect_return = false);

private:
   std::unordered_map<HookType::hook_type_t, std::vector<HookCallback> > m_registry;
};

#endif /* __HOOKS_MANAGER_H */
