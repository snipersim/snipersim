#ifndef __MMU_PERF_MODEL_BASE_H__
#define __MMU_PERF_MODEL_BASE_H__

#include "config.h"
#include "log.h"


class MMUPerfModelBase
{
   protected:
      UInt32 shmem_send_message_delay;
      UInt32 shmem_receive_message_delay;


   public:
      MMUPerfModelBase() 
      {
         // shmem_send_message_delay = Config::getSingleton()->getShMemSendMessageDelay();
         // shmem_receive_message_delay = Config::getSingleton()->getShMemReceiveMessageDelay();
      }

      virtual ~MMUPerfModelBase() { }

      static MMUPerfModelBase* createModel(UInt32 type);

      enum MMUActions_t 
      {
         SEND_MESSAGE,     
         RECEIVE_MESSAGE,  
         NUM_MMU_ACTIONS
      };

      enum MMUPerfModel_t
      {
         MMU_PERF_MODEL = 0,
         NUM_MMU_PERF_MODELS
      };

      virtual UInt32 getLatency(MMUActions_t action)
      {
         return 0;
      }

};

#endif /* __MMU_PERF_MODEL_BASE_H__ */
