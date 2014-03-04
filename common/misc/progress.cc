#include "progress.h"
#include "simulator.h"
#include "config.hpp"
#include "core_manager.h"
#include "hooks_manager.h"
#include "trace_manager.h"
#include "magic_server.h"

Progress::Progress()
   : m_enabled(false)
   , m_t_last(0)
   , m_manual(false)
   , m_manual_value(0.f)
{
   String filename = Sim()->getCfg()->getString("progress_trace/filename");

   if (!filename.empty())
   {
      m_fp = fopen(filename.c_str(), "w");
      m_enabled = true;

      Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC_INS, __record, (UInt64)this);
   }
}

Progress::~Progress(void)
{
   if (m_enabled)
   {
      fclose(m_fp);
   }
}

void Progress::setProgress(float progress)
{
   m_manual = true;
   m_manual_value = progress;
}

void Progress::record(UInt64 simtime)
{
   if (m_t_last + m_interval < time(NULL))
   {
      m_t_last = time(NULL);

      UInt64 expect = 0;

      // Always return global instruction count, so MIPS number as reported by job infrastructure is correct
      UInt64 progress = MagicServer::getGlobalInstructionCount();

      if (m_manual && m_manual_value > 0)
      {
         // Re-compute expected completion based on current progress
         expect = progress / m_manual_value;
      }
      else if (Sim()->getTraceManager())
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

      rewind(m_fp);
      fprintf(m_fp, "%u %" PRId64 " %" PRId64, unsigned(time(NULL)), progress, expect);
      fflush(m_fp);
   }
}
