#include "simulator.h"
#include "core_manager.h"
#include "config.h"
#include "core.h"
#include "log.h"

CAPI_return_t CAPI_rank(int *core_id)
{
   *core_id = Sim()->getCoreManager()->getCurrentCoreID();
   return 0;
}

CAPI_return_t CAPI_Initialize(int rank)
{
   Sim()->getCoreManager()->initializeCommId(rank);
   return 0;
}

CAPI_return_t CAPI_message_send_w(CAPI_endpoint_t sender,
      CAPI_endpoint_t receiver,
      char *buffer,
      int size)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();

   LOG_PRINT("SimSendW - sender: %d, recv: %d, size: %d", sender, receiver, size);

   core_id_t sending_core = Config::getSingleton()->getCoreFromCommId(sender);

   core_id_t receiving_core = CAPI_ENDPOINT_ALL;
   if (receiver != CAPI_ENDPOINT_ALL)
      receiving_core = Config::getSingleton()->getCoreFromCommId(receiver);

   if(sending_core == INVALID_CORE_ID)
       return CAPI_SenderNotInitialized;
   if(receiving_core == INVALID_CORE_ID)
       return CAPI_ReceiverNotInitialized;

   return core ? core->coreSendW(sending_core, receiving_core, buffer, size, (carbon_network_t) CARBON_NET_USER_1) : CAPI_SenderNotInitialized;
}

CAPI_return_t CAPI_message_send_w_ex(CAPI_endpoint_t sender,
      CAPI_endpoint_t receiver,
      char *buffer,
      int size,
      carbon_network_t net_type)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();

   LOG_PRINT("SimSendW - sender: %d, recv: %d, size: %d", sender, receiver, size);

   core_id_t sending_core = Config::getSingleton()->getCoreFromCommId(sender);

   core_id_t receiving_core = CAPI_ENDPOINT_ALL;
   if (receiver != CAPI_ENDPOINT_ALL)
      receiving_core = Config::getSingleton()->getCoreFromCommId(receiver);

   if(sending_core == INVALID_CORE_ID)
       return CAPI_SenderNotInitialized;
   if(receiving_core == INVALID_CORE_ID)
       return CAPI_ReceiverNotInitialized;

   return core ? core->coreSendW(sending_core, receiving_core, buffer, size, net_type) : CAPI_SenderNotInitialized;
}

CAPI_return_t CAPI_message_receive_w(CAPI_endpoint_t sender,
      CAPI_endpoint_t receiver,
      char *buffer,
      int size)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();

   LOG_PRINT("SimRecvW - sender: %d, recv: %d, size: %d", sender, receiver, size);

   core_id_t sending_core = CAPI_ENDPOINT_ANY;
   if (sender != CAPI_ENDPOINT_ANY)
      sending_core = Config::getSingleton()->getCoreFromCommId(sender);

   core_id_t receiving_core = Config::getSingleton()->getCoreFromCommId(receiver);

   if(sending_core == INVALID_CORE_ID)
       return CAPI_SenderNotInitialized;
   if(receiving_core == INVALID_CORE_ID)
       return CAPI_ReceiverNotInitialized;

   return core ? core->coreRecvW(sending_core, receiving_core, buffer, size, (carbon_network_t) CARBON_NET_USER_1) : CAPI_ReceiverNotInitialized;
}

CAPI_return_t CAPI_message_receive_w_ex(CAPI_endpoint_t sender,
      CAPI_endpoint_t receiver,
      char *buffer,
      int size,
      carbon_network_t net_type)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();

   LOG_PRINT("SimRecvW - sender: %d, recv: %d, size: %d", sender, receiver, size);

   core_id_t sending_core = CAPI_ENDPOINT_ANY;
   if (sender != CAPI_ENDPOINT_ANY)
      sending_core = Config::getSingleton()->getCoreFromCommId(sender);

   core_id_t receiving_core = Config::getSingleton()->getCoreFromCommId(receiver);

   if(sending_core == INVALID_CORE_ID)
       return CAPI_SenderNotInitialized;
   if(receiving_core == INVALID_CORE_ID)
       return CAPI_ReceiverNotInitialized;

   return core ? core->coreRecvW(sending_core, receiving_core, buffer, size, net_type) : CAPI_ReceiverNotInitialized;
}
