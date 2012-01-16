#ifndef SMTRANSPORT_H
#define SMTRANSPORT_H

#include <queue>

#include "transport.h"
#include "cond.h"

class SmTransport : public Transport
{
public:
   SmTransport();
   ~SmTransport();

   class SmNode : public Node
   {
   public:
      SmNode(core_id_t core_id, SmTransport *smt);
      ~SmNode();

      void globalSend(SInt32, const void*, UInt32);
      void send(core_id_t, const void*, UInt32);
      Byte* recv();
      bool query();

   private:
      void send(SmNode *dest, const void *buffer, UInt32 length);

      std::queue<Byte*> m_queue;
      Lock m_lock;
      ConditionVariable m_cond;
      SmTransport *m_smt;
   };

   Node* createNode(core_id_t core_id);

   void barrier();
   Node* getGlobalNode();

private:
   Node *m_global_node;
   SmNode **m_core_nodes;

   SmNode *getNodeFromId(core_id_t core_id);
   void clearNodeForId(core_id_t core_id);
};

#endif
