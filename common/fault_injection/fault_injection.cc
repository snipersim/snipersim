#include "fault_injection.h"
#include "fault_injector_random.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"

FaultinjectionManager *
FaultinjectionManager::create()
{
   String s_type = Sim()->getCfg()->getString("fault_injection/type");
   String s_injector = Sim()->getCfg()->getString("fault_injection/injector");

   fault_type_t type;
   if (s_type == "toggle")
      type = FAULT_TYPE_TOGGLE;
   else if (s_type == "set0")
      type = FAULT_TYPE_SET0;
   else if (s_type == "set1")
      type = FAULT_TYPE_SET1;
   else if (s_type == "none")
      return NULL;
   else
      LOG_PRINT_ERROR("Unknown fault-injection scheme %s", s_type.c_str());

   fault_injector_t injector;
   if (s_injector == "none")
      injector = FAULT_INJECTOR_NONE;
   else if (s_injector == "random")
      injector = FAULT_INJECTOR_RANDOM;
   else
      LOG_PRINT_ERROR("Unknown fault injector %s", s_injector.c_str());

   return new FaultinjectionManager(type, injector);
}

FaultinjectionManager::FaultinjectionManager(fault_type_t type, fault_injector_t injector)
   : m_type(type)
   , m_injector(injector)
{
}

FaultInjector *
FaultinjectionManager::getFaultInjector(UInt32 core_id, MemComponent::component_t mem_component)
{
   switch(m_injector)
   {
      case FAULT_INJECTOR_NONE:
         return new FaultInjector(core_id, mem_component);
      case FAULT_INJECTOR_RANDOM:
         return new FaultInjectorRandom(core_id, mem_component);
   }

   return NULL;
}

void
FaultinjectionManager::applyFault(Core *core, IntPtr read_address, UInt32 data_size, MemoryResult &memres, Byte *data, const Byte *fault)
{
   switch(m_type)
   {
      case FAULT_TYPE_TOGGLE:
         for(UInt32 i = 0; i < data_size; ++i)
            data[i] ^= fault[i];
         break;
      case FAULT_TYPE_SET0:
         for(UInt32 i = 0; i < data_size; ++i)
            data[i] &= ~fault[i];
         break;
      case FAULT_TYPE_SET1:
         for(UInt32 i = 0; i < data_size; ++i)
            data[i] |= fault[i];
         break;
   }
}

FaultInjector::FaultInjector(UInt32 core_id, MemComponent::component_t mem_component)
   : m_core_id(core_id)
   , m_mem_component(mem_component)
{
}

void
FaultInjector::preRead(IntPtr addr, IntPtr location, UInt32 data_size, Byte *fault, SubsecondTime time)
{
   // Data at virtual address <addr> is about to be read with size <data_size>.
   // <location> corresponds to the physical location (cache line) where the data lives.
   // Update <fault> here according to errors that have accumulated in this memory location.
}

void
FaultInjector::postWrite(IntPtr addr, IntPtr location, UInt32 data_size, Byte *fault, SubsecondTime time)
{
   // Data at virtual address <addr> has just been written to.
   // Update <fault> here according to errors that occured during the writing of this memory location.
}
