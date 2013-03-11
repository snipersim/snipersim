#include "mem_component.h"

const char * MemComponentString(MemComponent::component_t mem_component)
{
   switch(mem_component)
   {
      case MemComponent::CORE:         return "core";
      case MemComponent::L1_ICACHE:    return "l1i";
      case MemComponent::L1_DCACHE:    return "l1d";
      case MemComponent::L2_CACHE:     return "l2";
      case MemComponent::L3_CACHE:     return "l3";
      case MemComponent::L4_CACHE:     return "l4";
      case MemComponent::TAG_DIR:      return "directory";
      case MemComponent::NUCA_CACHE:   return "nuca-cache";
      case MemComponent::DRAM_CACHE:   return "dram-cache";
      case MemComponent::DRAM:         return "dram";
      default:                         return "????";
   }
}
