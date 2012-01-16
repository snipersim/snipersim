#include "progress.h"
#include "simulator.h"
#include "config.hpp"
#include "core_manager.h"
#include "hooks_manager.h"

bool progress_enabled = false;
FILE * progress_fp;
time_t progress_t_last = 0;
const time_t progress_interval = 2;

void Progress::init(void)
{
   String progress_file = Sim()->getCfg()->getString("progress_trace/filename", "");
   if (!progress_file.empty()) {
      progress_fp = fopen(progress_file.c_str(), "w");
      progress_enabled = true;
      Progress::record(true, SubsecondTime::Zero());

      Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC, (HooksManager::HookCallbackFunc)Progress::record, (void*)false);
   }
}

void Progress::fini(void)
{
   if (progress_enabled) {
      Progress::record(false, SubsecondTime::Zero());
      fclose(progress_fp);
   }
}

void Progress::record(bool init, subsecond_time_t simtime)
{
   if (progress_t_last + progress_interval < time(NULL)) {
      progress_t_last = time(NULL);

      UInt64 ninstrs = 0;
      if (! init)
         for(core_id_t id = 0; id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++id)
            ninstrs += Sim()->getCoreManager()->getCoreFromID(id)->getInstructionCount();

      rewind(progress_fp);
      fprintf(progress_fp, "%u %lu %ld", unsigned(time(NULL)), ninstrs, SubsecondTime(simtime).getNS());
      fflush(progress_fp);
   }
}
