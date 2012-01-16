#ifndef __MMU_PERF_MODEL_H__
#define __MMU_PERF_MODEL_H__

#include "mmu_perf_model_base.h"
#include "log.h"

class MMUPerfModel : public MMUPerfModelBase
{
   private:

   public:
      MMUPerfModel () : MMUPerfModelBase() { }
      ~MMUPerfModel() { }

      UInt32 getLatency(MMUActions_t action)
      {
         switch(action)
         {
            case SEND_MESSAGE:
               return shmem_send_message_delay;

            case RECEIVE_MESSAGE:
               return shmem_receive_message_delay;

            default:
               LOG_ASSERT_ERROR(false, "Unsupported MMU Action Type: %u", action);
               return 0;
         }
      }
};

#endif /* __MMU_PERF_MODEL_H__ */
