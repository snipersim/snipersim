#include "fault_injector_random.h"
#include "rng.h"

FaultInjectorRandom::FaultInjectorRandom(UInt32 core_id, MemComponent::component_t mem_component)
   : FaultInjector(core_id, mem_component)
   , m_rng(rng_seed(0))
{
   if (mem_component == MemComponent::L1_DCACHE)
      m_active = true;
   else
      m_active = false;
}

void
FaultInjectorRandom::preRead(IntPtr addr, IntPtr location, UInt32 data_size, Byte *fault, SubsecondTime time)
{
   // Data at virtual address <addr> is about to be read with size <data_size>.
   // <location> corresponds to the physical location (cache line) where the data lives.
   // Update <fault> here according to errors that have accumulated in this memory location.

   // Dummy random fault injector
   if (m_active && (rng_next(m_rng) % 0xffff) == 0)
   {
      UInt32 bit_location = rng_next(m_rng) % data_size;

      printf("Inserting bit %d flip at address %" PRIxPTR " on read access by core %d to component %s\n",
         bit_location, addr, m_core_id, MemComponentString(m_mem_component));

      fault[bit_location / 8] |= 1 << (bit_location % 8);
   }
}

void
FaultInjectorRandom::postWrite(IntPtr addr, IntPtr location, UInt32 data_size, Byte *fault, SubsecondTime time)
{
   // Data at virtual address <addr> has just been written to.
   // Update <fault> here according to errors that occured during the writing of this memory location.
}
