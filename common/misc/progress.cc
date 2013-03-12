#include "progress.h"
#include "simulator.h"
#include "config.hpp"
#include "core_manager.h"
#include "hooks_manager.h"
#include "trace_manager.h"
#include "magic_server.h"

bool progress_enabled = false;
FILE * progress_fp;
time_t progress_t_last = 0;
const time_t progress_interval = 2;

void Progress::init(void)
{
   String progress_file = Sim()->getCfg()->getString("progress_trace/filename");
   if (!progress_file.empty()) {
      progress_fp = fopen(progress_file.c_str(), "w");
      progress_enabled = true;
      Progress::record(true, 0);

      Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC_INS, Progress::record, (UInt64)false);
   }
}

void Progress::fini(void)
{
   if (progress_enabled) {
      Progress::record(false, 0);
      fclose(progress_fp);
   }
}

SInt64 Progress::record(UInt64 init, UInt64 simtime)
{
   if (progress_t_last + progress_interval < time(NULL)) {
      progress_t_last = time(NULL);

      UInt64 expect = 0;

      // Always return global instruction count, so MIPS number as reported by job infrastructure is correct
      UInt64 progress = MagicServer::getGlobalInstructionCount();

      if (Sim()->getTraceManager())
      {
         UInt64 _expect = Sim()->getTraceManager()->getProgressExpect();
         UInt64 _progress = Sim()->getTraceManager()->getProgressValue();

         // Re-compute expected completion based on file-pointer based % to completion,
         // and the progress value (instruction count) we'll return
         if (_progress > 1)
            expect = progress * _expect / _progress;
         else
            expect = 100000 * progress;
      }

      rewind(progress_fp);
      fprintf(progress_fp, "%u %" PRId64 " %" PRId64, unsigned(time(NULL)), progress, expect);
      fflush(progress_fp);
   }

   return 0;
}
