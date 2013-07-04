#pragma once

#include <map>
#include <queue>

#include "log.h"


// Define to use the alternate, fixed-length queue implementation
//#define ENABLE_CIRCULAR_QUEUE


#ifndef ENABLE_CIRCULAR_QUEUE

template <class T_Req> class ReqQueueListTemplate
{
   private:
      std::map<IntPtr, std::queue<T_Req*>* > m_req_queue_list;

   public:
      ReqQueueListTemplate() {};
      ~ReqQueueListTemplate() {};

      void enqueue(IntPtr address, T_Req* shmem_req);
      T_Req* dequeue(IntPtr address);
      T_Req* front(IntPtr address);
      T_Req* back(IntPtr address);
      UInt32 size(IntPtr address);
      bool empty(IntPtr address);
};

template <class T_Req>
void
ReqQueueListTemplate<T_Req>::enqueue(IntPtr address, T_Req* shmem_req)
{
   if (m_req_queue_list.count(address) == 0)
   {
      m_req_queue_list[address] = new std::queue<T_Req*>();
   }
   m_req_queue_list[address]->push(shmem_req);
}

template <class T_Req>
T_Req*
ReqQueueListTemplate<T_Req>::dequeue(IntPtr address)
{
   LOG_ASSERT_ERROR(m_req_queue_list.count(address) != 0,
         "Could not find a request with address(0x%x)", address);

   T_Req* shmem_req = m_req_queue_list[address]->front();
   m_req_queue_list[address]->pop();
   if (m_req_queue_list[address]->empty())
   {
      delete m_req_queue_list[address];
      m_req_queue_list.erase(address);
   }
   return shmem_req;
}

template <class T_Req>
T_Req*
ReqQueueListTemplate<T_Req>::front(IntPtr address)
{
   LOG_ASSERT_ERROR(m_req_queue_list.count(address) != 0,
         "Could not find a request with address(0x%x)", address);

   return m_req_queue_list[address]->front();
}

template <class T_Req>
T_Req*
ReqQueueListTemplate<T_Req>::back(IntPtr address)
{
   LOG_ASSERT_ERROR(m_req_queue_list.count(address) != 0,
         "Could not find a request with address(0x%x)", address);

   return m_req_queue_list[address]->back();
}

template <class T_Req>
UInt32
ReqQueueListTemplate<T_Req>::size(IntPtr address)
{
   if (m_req_queue_list.count(address) == 0)
      return 0;
   else
      return m_req_queue_list[address]->size();
}

template <class T_Req>
bool
ReqQueueListTemplate<T_Req>::empty(IntPtr address)
{
   if (m_req_queue_list.count(address) == 0)
      return true;
   else
      return false;
}


#else // ENABLE_CIRCULAR_QUEUE


#include "circular_queue.h"

template <class T_Req> class ReqQueueListTemplate
{
   private:
      std::map<IntPtr, CircularQueue<T_Req*> > m_req_queue_list;

   public:
      ReqQueueListTemplate() {};
      ~ReqQueueListTemplate() {};

      void enqueue(IntPtr address, T_Req* shmem_req);
      T_Req* dequeue(IntPtr address);
      T_Req* front(IntPtr address);
      UInt32 size(IntPtr address);
      bool empty(IntPtr address);
};

template <class T_Req>
void
ReqQueueListTemplate<T_Req>::enqueue(IntPtr address, T_Req* shmem_req)
{
   m_req_queue_list[address].push(shmem_req);
}

template <class T_Req>
T_Req*
ReqQueueListTemplate<T_Req>::dequeue(IntPtr address)
{
   LOG_ASSERT_ERROR(m_req_queue_list.count(address) && ! m_req_queue_list[address].empty(),
         "Could not find a request with address(0x%x)", address);

   T_Req* shmem_req = m_req_queue_list[address].pop();
   if (m_req_queue_list[address].empty())
      m_req_queue_list.erase(address);
   return shmem_req;
}

template <class T_Req>
T_Req*
ReqQueueListTemplate<T_Req>::front(IntPtr address)
{
   LOG_ASSERT_ERROR(m_req_queue_list.count(address) && ! m_req_queue_list[address].empty(),
         "Could not find a request with address(0x%x)", address);

   return m_req_queue_list[address].front();
}

template <class T_Req>
UInt32
ReqQueueListTemplate<T_Req>::size(IntPtr address)
{
   if (m_req_queue_list.count(address) == 0)
      return 0;
   else
      return m_req_queue_list[address].size();
}

template <class T_Req>
bool
ReqQueueListTemplate<T_Req>::empty(IntPtr address)
{
   if (m_req_queue_list.count(address) == 0 || m_req_queue_list[address].empty())
      return true;
   else
      return false;
}


#endif // ENABLE_CIRCULAR_QUEUE
