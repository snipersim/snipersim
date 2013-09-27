#ifndef __ALLOCATOR_H
#define __ALLOCATOR_H

#include "fixed_types.h"
#include "FSBAllocator.hh"

#include <typeinfo>
#include <cxxabi.h>

// Pool allocator

class Allocator
{
   private:
      struct DataElement
      {
          Allocator *allocator;
          char data[];
      };
   public:
      virtual ~Allocator() {}

      virtual void *alloc(size_t bytes) = 0;
      virtual void _dealloc(void *ptr) = 0;

      static void dealloc(void* ptr)
      {
         DataElement *elem = (DataElement*)(((char*)ptr) - sizeof(DataElement));
         elem->allocator->_dealloc(elem);
      }
};

template <typename T, unsigned MaxItems = 0> class TypedAllocator : public Allocator
{
   private:
      UInt64 m_items;
      FSBAllocator_ElemAllocator<sizeof(DataElement) + sizeof(T), MaxItems, T> m_alloc;

      // In ROB-SMT, DynamicMicroOps are allocated by their own thread but free'd in simulate() which can be called by anyone
      Lock m_lock;

   public:
      TypedAllocator()
         : m_items(0)
      {}

      virtual ~TypedAllocator()
      {
         if (m_items)
         {
            int status;
            char *nameoftype = abi::__cxa_demangle(typeid(T).name(), 0, 0, &status);
            printf("[ALLOC] %" PRIu64 " items of type %s not freed\n", m_items, nameoftype);
            free(nameoftype);
         }
      }

      virtual void* alloc(size_t bytes)
      {
         ScopedLock sl(m_lock);
         //LOG_ASSERT_ERROR(bytes == sizeof(T), "");
         ++m_items;
         DataElement *elem = (DataElement *)m_alloc.allocate();
         elem->allocator = this;
         return elem->data;
      }

      virtual void _dealloc(void* ptr)
      {
         ScopedLock sl(m_lock);
         --m_items;
         m_alloc.deallocate((T*)ptr);
      }
};

#endif // __ALLOCATOR_H
