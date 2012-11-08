#include "fault_injector_random.h"

FaultInjectorRandom::FaultInjectorRandom(UInt32 core_id, MemComponent::component_t mem_component)
   : FaultInjector(core_id, mem_component)
{
}

void
FaultInjectorRandom::preRead(IntPtr addr, IntPtr location, UInt32 data_size, Byte *fault, SubsecondTime time)
{
   // Data at virtual address <addr> is about to be read with size <data_size>.
   // <location> corresponds to the physical location (cache line) where the data lives.
   // Update <fault> here according to errors that have accumulated in this memory location.
}

void
FaultInjectorRandom::postWrite(IntPtr addr, IntPtr location, UInt32 data_size, Byte *fault, SubsecondTime time)
{
   // Data at virtual address <addr> has just been written to.
   // Update <fault> here according to errors that occured during the writing of this memory location.
}
