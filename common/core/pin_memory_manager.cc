#include <stdlib.h>
#include "pin_memory_manager.h"

PinMemoryManager::PinMemoryManager(Core* core):
   m_core(core)
{
   // Allocate scratchpads (aligned at 4 * sizeof (void*) to correctly handle
   // memory access instructions that require addresses to be aligned such as
   // MOVDQA
   for (unsigned int i = 0; i < NUM_ACCESS_TYPES; i++)
   {
      int status = posix_memalign ((void**) &m_scratchpad[i], 4 * sizeof (void*), m_scratchpad_size);
      assert (status == 0);
   }
}

PinMemoryManager::~PinMemoryManager()
{
   for (unsigned int i = 0; i < NUM_ACCESS_TYPES; i++)
      free(m_scratchpad[i]);
}

carbon_reg_t
PinMemoryManager::redirectMemOp (IntPtr eip, bool has_lock_prefix, IntPtr tgt_ea, IntPtr size, UInt32 op_num, bool is_read)
{
   assert (op_num < NUM_ACCESS_TYPES);
   char *scratchpad = m_scratchpad [op_num];

   if (is_read)
   {
      Core::mem_op_t mem_op_type;
      Core::lock_signal_t lock_signal;

      if (has_lock_prefix)
      {
         // FIXME: Now, when we have a LOCK prefix, we do an exclusive READ
         mem_op_type = Core::READ_EX;
         lock_signal = Core::LOCK;
      }
      else
      {
         mem_op_type = Core::READ;
         lock_signal = Core::NONE;
      }

      m_core->accessMemory (lock_signal, mem_op_type, tgt_ea, scratchpad, size, Core::MEM_MODELED_DYNINFO, eip);

   }
   return (carbon_reg_t) scratchpad;
}

void
PinMemoryManager::completeMemWrite (IntPtr eip, bool has_lock_prefix, IntPtr tgt_ea, IntPtr size, UInt32 op_num)
{
   char *scratchpad = m_scratchpad [op_num];

   Core::lock_signal_t lock_signal = (has_lock_prefix) ? Core::UNLOCK : Core::NONE;

   m_core->accessMemory (lock_signal, Core::WRITE, tgt_ea, scratchpad, size, Core::MEM_MODELED_DYNINFO, eip);

   return;
}

carbon_reg_t
PinMemoryManager::redirectPushf (IntPtr eip, IntPtr tgt_esp, IntPtr size )
{
   m_saved_esp = tgt_esp;
   return ((carbon_reg_t) m_scratchpad [0]) + size;
}

carbon_reg_t
PinMemoryManager::completePushf (IntPtr eip, IntPtr esp, IntPtr size )
{
   m_saved_esp -= size;
   completeMemWrite (eip, false, (IntPtr) m_saved_esp, size, 0);
   return m_saved_esp;
}

carbon_reg_t
PinMemoryManager::redirectPopf (IntPtr eip, IntPtr tgt_esp, IntPtr size)
{
   m_saved_esp = tgt_esp;
   return redirectMemOp (eip, false, m_saved_esp, size, 0, true);
}

carbon_reg_t
PinMemoryManager::completePopf (IntPtr eip, IntPtr esp, IntPtr size)
{
   return (m_saved_esp + size);
}
