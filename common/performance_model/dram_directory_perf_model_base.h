#ifndef __DRAM_DIRECTORY_PERF_MODEL_BASE_H__
#define __DRAM_DIRECTORY_PERF_MODEL_BASE_H__

#include "log.h"
#include "config.hpp"

#include "simulator.h"

class DramDirectoryPerfModelBase
{
   protected:
      UInt32 access_dir_cache_delay;
      UInt32 enqueue_request_delay;
      UInt32 dequeue_request_delay;
      UInt32 process_request_delay;
      UInt32 process_ack_delay;
      UInt32 shmem_send_message_delay;
      UInt32 shmem_receive_message_delay;


   public:
      DramDirectoryPerfModelBase() 
      {
         access_dir_cache_delay = Sim()->getCfg()->getInt("perf_model/dram_dir/access_time");
         // enqueue_request_delay = Config::getSingleton()->getEnqueueRequestDelay();
         // dequeue_request_delay = Config::getSingleton()->getDequeueRequestDelay();
         // process_request_delay = Config::getSingleton()->getProcessRequestDelay();
         // process_ack_delay = Config::getSingleton()->getProcessAckDelay();
         // shmem_send_message_delay = Config::getSingleton()->getShMemSendMessageDelay();
         // shmem_receive_message_delay = Config::getSingleton()->getShMemReceiveMessageDelay();
      }

      virtual ~DramDirectoryPerfModelBase() { }

      static DramDirectoryPerfModelBase* createModel(UInt32 type);

      enum DramDirActions_t 
      {
         ACCESS_DIR_CACHE = 0,
         ENQUEUE_REQUEST,
         DEQUEUE_REQUEST,
         PROCESS_REQUEST,
         PROCESS_ACK,
         SEND_MESSAGE,     
         RECEIVE_MESSAGE,  
         NUM_DRAM_DIR_ACTIONS
      };

      enum DramDirectoryPerfModel_t
      {
         DRAM_DIRECTORY_PERF_MODEL = 0,
         NUM_DRAM_DIRECTORY_PERF_MODELS
      };

      virtual UInt32 getLatency(DramDirActions_t action)
      {
         return 0;
      }

};

#endif /* __DRAM_DIRECTORY_PERF_MODEL_BASE_H__ */
