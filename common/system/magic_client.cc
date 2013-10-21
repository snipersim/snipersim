#include "magic_client.h"
#include "magic_server.h"
#include "sim_api.h"
#include "simulator.h"
#include "core.h"
#include "core_manager.h"
#include "thread.h"
#include "thread_manager.h"

static UInt64 handleMagic(thread_id_t thread_id, UInt64 cmd, UInt64 arg0 = 0, UInt64 arg1 = 0)
{
   Thread *thread = (thread_id == INVALID_THREAD_ID) ? NULL : Sim()->getThreadManager()->getThreadFromID(thread_id);
   Core *core = thread == NULL ? NULL : thread->getCore();
   return Sim()->getMagicServer()->Magic(thread_id, core ? core->getId() : INVALID_CORE_ID, cmd, arg0, arg1);
}

void setInstrumentationMode(UInt64 opt)
{
   handleMagic(INVALID_THREAD_ID, SIM_CMD_INSTRUMENT_MODE, opt);
}

UInt64 handleMagicInstruction(thread_id_t thread_id, UInt64 cmd, UInt64 arg0, UInt64 arg1)
{
   switch(cmd)
   {
   case SIM_CMD_ROI_TOGGLE:
   case SIM_CMD_ROI_START:
   case SIM_CMD_ROI_END:
   case SIM_CMD_MHZ_SET:
   case SIM_CMD_MARKER:
   case SIM_CMD_NAMED_MARKER:
   case SIM_CMD_USER:
   case SIM_CMD_INSTRUMENT_MODE:
   case SIM_CMD_MHZ_GET:
   case SIM_CMD_SET_THREAD_NAME:
      return handleMagic(thread_id, cmd, arg0, arg1);
   case SIM_CMD_PROC_ID:
   {
      Core *core = Sim()->getCoreManager()->getCurrentCore();
      return core->getId();
   }
   case SIM_CMD_THREAD_ID:
      return thread_id;
   case SIM_CMD_NUM_PROCS:
      return Sim()->getConfig()->getApplicationCores();
   case SIM_CMD_NUM_THREADS:
      return Sim()->getThreadManager()->getNumThreads();
   case SIM_CMD_IN_SIMULATOR:
      return 0;
   default:
      LOG_PRINT_WARNING_ONCE("Encountered unknown magic instruction cmd(%u)", cmd);
      return 1;
   }
}
