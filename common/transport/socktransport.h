#ifndef SOCK_TRANSPORT_H
#define SOCK_TRANSPORT_H

#include "transport.h"
#include "thread.h"
#include "semaphore.h"

#include <list>

class SockTransport : public Transport
{
public:
   SockTransport();
   ~SockTransport();
   
   class SockNode : public Node
   {
   public:
      SockNode(core_id_t core_id, SockTransport *trans);
      ~SockNode();

      void globalSend(SInt32 dest_proc, const void *buffer, UInt32 length);
      void send(core_id_t dest_core, const void *buffer, UInt32 length);
      Byte* recv();
      bool query();

   private:
      void send(SInt32 dest_proc, UInt32 tag, const void *buffer, UInt32 length);

      SockTransport *m_transport;
   };

   Node *createNode(core_id_t core_id);

   void barrier();
   Node *getGlobalNode();

private:
   struct Packet
   {
      UInt32 length;
      SInt32 tag;
      Byte data;
   } __attribute__((__packed__));

   struct Header
   {
      Header(UInt32 length, UInt64 checksum):
         m_length(length), m_checksum(checksum) {}

      UInt32 m_length;
      UInt64 m_checksum;
   };
   
   void getProcInfo();
   void initSockets();
   void initBufferLists();
   void insertInBufferList(SInt32 tag, Byte *buffer, Header* header = NULL);

   static void updateThreadFunc(void *vp);
   void updateBufferLists();
   void terminateUpdateThread();

   class Socket
   {
   public:
      Socket();
      ~Socket();

      // server api
      void listen(SInt32 port, SInt32 max_pending);
      Socket accept();

      // client api
      void connect(const char *addr, SInt32 port);

      void send(const void* buffer, UInt32 length);
      bool recv(void *buffer, UInt32 length, bool block);

      void close();

   private:
      Socket(SInt32);

      SInt32 m_socket;
   };

   enum UpdateThreadState
   {
      RUNNING,
      EXITING,
      EXITED
   };

   static const SInt32 DEFAULT_BASE_PORT = 2000;
   static const SInt32 GLOBAL_TAG = -1;
   static const SInt32 BARRIER_TAG = -2;
   static const SInt32 TERMINATE_TAG = -3;

   Node *m_global_node;

   SInt32 m_base_port;
   SInt32 m_num_procs;
   SInt32 m_proc_index;

   Semaphore m_barrier_sem;

   Socket m_server_socket;
   Lock *m_recv_locks;
   Socket *m_recv_sockets;
   Lock *m_send_locks;
   Socket *m_send_sockets;

   Thread *m_update_thread;
   UpdateThreadState m_update_thread_state;

   typedef std::list<Byte*> buffer_list;
   SInt32 m_num_lists;
   buffer_list *m_buffer_lists;
   std::list<Header*> *m_header_lists;

   Lock *m_buffer_list_locks;
   Semaphore *m_buffer_list_sems;
};

#endif // SOCK_TRANSPORT_H
