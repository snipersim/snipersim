#include <string.h>

#include "smtransport.h"
#include "config.h"
#include "log.h"

// -- SmTransport -- //

SmTransport::SmTransport()
{
   m_global_node = new SmNode(-1, this);
   m_core_nodes = new SmNode* [ Config::getSingleton()->getTotalCores() ];
   for (UInt32 i = 0; i < Config::getSingleton()->getTotalCores(); i++)
      m_core_nodes[i] = NULL;
}

SmTransport::~SmTransport()
{
   // The networks actually delete the Transport::Nodes, so we
   // shouldn't do it ourselves.
   // for (UInt32 i = 0; i < Config::getSingleton()->getNumLocalCores(); i++)
   //    delete m_core_nodes[i];

   delete [] m_core_nodes;
   delete m_global_node;
}

Transport::Node* SmTransport::createNode(core_id_t core_id)
{
   LOG_ASSERT_ERROR((UInt32)core_id < Config::getSingleton()->getTotalCores(),
                    "Request index out of range: %d", core_id);
   LOG_ASSERT_ERROR(m_core_nodes[core_id] == NULL,
                    "Transport already allocated for id: %d.", core_id);

   m_core_nodes[core_id] = new SmNode(core_id, this);

   LOG_PRINT("Created node: %p on id: %d", m_core_nodes[core_id], core_id);

   return m_core_nodes[core_id];
}

void SmTransport::barrier()
{
   // We assume a single process, so this is a NOOP
}

Transport::Node* SmTransport::getGlobalNode()
{
   return m_global_node;
}

SmTransport::SmNode* SmTransport::getNodeFromId(core_id_t core_id)
{
   LOG_ASSERT_ERROR((UInt32)core_id < Config::getSingleton()->getTotalCores(),
                    "Core id out of range: %d", core_id);
   return m_core_nodes[core_id];
}

void SmTransport::clearNodeForId(core_id_t core_id)
{
   // This is called upon deletion of the node, so we should simply
   // not keep around a dead pointer.
   if ((UInt32)core_id < Config::getSingleton()->getTotalCores())
      m_core_nodes[core_id] = NULL;
}

// -- SmTransportNode -- //

SmTransport::SmNode::SmNode(core_id_t core_id, SmTransport *smt)
   : Node(core_id)
   , m_smt(smt)
{
}

SmTransport::SmNode::~SmNode()
{
   LOG_ASSERT_WARNING(m_queue.empty(), "Unread messages in queue for core: %d", getCoreId());
   m_smt->clearNodeForId(getCoreId());
}

void SmTransport::SmNode::globalSend(SInt32 dest_proc, const void *buffer, UInt32 length)
{
   LOG_ASSERT_ERROR(dest_proc == 0, "Destination other than zero: %d", dest_proc);
   send((SmNode*)m_smt->getGlobalNode(), buffer, length);
}

void SmTransport::SmNode::send(SInt32 dest_id, const void* buffer, UInt32 length)
{
   SmNode *dest_node = m_smt->getNodeFromId(dest_id);
   LOG_ASSERT_ERROR(dest_node != NULL, "Attempt to send to non-existent node: %d", dest_id);
   send(dest_node, buffer, length);
}

void SmTransport::SmNode::send(SmNode *dest_node, const void *buffer, UInt32 length)
{
   Byte *data = new Byte[length];
   memcpy(data, buffer, length);

   LOG_PRINT("sending msg -- size: %i, data: %p, dest: %p", length, data, dest_node);

   dest_node->m_lock.acquire();
   dest_node->m_queue.push(data);
   dest_node->m_lock.release();
   dest_node->m_cond.broadcast();
}

Byte* SmTransport::SmNode::recv()
{
   LOG_PRINT("attempting recv -- this: %p", this);

   m_lock.acquire();

   while (true)
   {
      if (!m_queue.empty())
      {
         Byte *data = m_queue.front();
         m_queue.pop();
         m_lock.release();

         LOG_PRINT("msg recv'd -- data: %p, this: %p", data, this);

         return data;
      }
      else
      {
         m_cond.wait(m_lock);
      }
   }
}

bool SmTransport::SmNode::query()
{
   bool result = false;

   m_lock.acquire();
   result = !m_queue.empty();
   m_lock.release();

   return result;
}
