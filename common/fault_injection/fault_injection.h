#ifndef __FAULT_INJECTION_H
#define __FAULT_INJECTION_H

#include "fixed_types.h"
#include "core.h"

class FaultInjector;

class FaultinjectionManager
{
   private:
      enum fault_type_t {
         FAULT_TYPE_TOGGLE,
         FAULT_TYPE_SET0,
         FAULT_TYPE_SET1,
      };
      fault_type_t m_type;

      enum fault_injector_t {
         FAULT_INJECTOR_NONE,
         FAULT_INJECTOR_RANDOM,
      };
      fault_injector_t m_injector;

   public:
      static FaultinjectionManager* create();

      FaultinjectionManager(fault_type_t type, fault_injector_t injector);

      FaultInjector* getFaultInjector(UInt32 core_id, MemComponent::component_t mem_component);

      void applyFault(Core *core, IntPtr read_address, UInt32 data_size, MemoryResult &memres, Byte *data, const Byte *fault);
};

class FaultInjector
{
   protected:
      UInt32 m_core_id;
      MemComponent::component_t m_mem_component;

   public:
      FaultInjector(UInt32 core_id, MemComponent::component_t mem_component);

      virtual void preRead(IntPtr addr, IntPtr location, UInt32 data_size, Byte *fault, SubsecondTime time);
      virtual void postWrite(IntPtr addr, IntPtr location, UInt32 data_size, Byte *fault, SubsecondTime time);
};

#endif // __FAULT_INJECTION_H
