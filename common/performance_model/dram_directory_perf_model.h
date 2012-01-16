#ifndef __DRAM_DIRECTORY_PERF_MODEL_H__
#define __DRAM_DIRECTORY_PERF_MODEL_H__

#include "dram_directory_perf_model_base.h"

class DramDirectoryPerfModel : public DramDirectoryPerfModelBase
{
   private:

   public:
      DramDirectoryPerfModel () : DramDirectoryPerfModelBase() { }
      ~DramDirectoryPerfModel() { }

      UInt32 getLatency(DramDirActions_t action)
      {
         switch(action)
         {
            case ACCESS_DIR_CACHE:
               return access_dir_cache_delay;

            case ENQUEUE_REQUEST:
               return enqueue_request_delay;

            case DEQUEUE_REQUEST:
               return dequeue_request_delay;

            case PROCESS_REQUEST:
               return process_request_delay;

            case PROCESS_ACK:
               return process_ack_delay;

            case SEND_MESSAGE:
               return shmem_send_message_delay;

            case RECEIVE_MESSAGE:
               return shmem_receive_message_delay;

            default:
               LOG_ASSERT_ERROR(false, "Unsupported Dram Action Type: %u", action);
               return 0;
         }
      }
 
};

#endif /* __DRAM_DIRECTORY_PERF_MODEL_H__ */
