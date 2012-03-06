#ifndef __TRACE_THREAD_H
#define __TRACE_THREAD_H

#include "fixed_types.h"
#include "barrier.h"
#include "thread.h"
#include "core.h"
#include "sift_reader.h"

#include <unordered_map>

class BasicBlock;

class TraceThread : public Runnable
{
   private:
      // In multi-process mode, we want each process to have its own private memory space
      // Therefore, perform a virtual to physical address mapping by including the core_id
      // Virtual addresses are converted to physical addresses by pasting the core_id at
      // bit positions pa_core_shift..+pa_core_size
      // The highest virtual address used is normally 00007fffffffffff, with pa_core_shift==48
      // the core_id is 2 above the highest used bit.
      static const intptr_t pa_core_shift = 48;
      static const intptr_t pa_core_size = 16;
      static const intptr_t pa_va_mask = ~(((intptr_t(1) << pa_core_size) - 1) << pa_core_shift);

      intptr_t va2pa(intptr_t va) { return (intptr_t(m_core->getId()) << pa_core_shift) | (va & pa_va_mask); }

      Thread *m_thread;
      Core *m_core;
      Sift::Reader m_trace;
      bool m_stop;
      Barrier *m_barrier;
      std::unordered_map<IntPtr, BasicBlock *> m_icache;
      IntPtr m_bbv_base;
      UInt64 m_bbv_count;
      xed_syntax_enum_t m_syntax;
      uint8_t m_output_leftover[160];
      uint16_t m_output_leftover_size;

      void run();
      static void __handleOutputFunc(void* arg, uint8_t fd, const uint8_t *data, uint32_t size)
      { ((TraceThread*)arg)->handleOutputFunc(fd, data, size); }
      void handleOutputFunc(uint8_t fd, const uint8_t *data, uint32_t size);
      BasicBlock* decode(Sift::Instruction &inst);

   public:
      TraceThread(core_id_t core_id, String tracefile);
      ~TraceThread();

      void spawn(Barrier *barrier);
      void stop() { m_stop = true; }
};

#endif // __TRACE_THREAD_H
