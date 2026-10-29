#ifndef TICKET_LOCK_H
#define TICKET_LOCK_H

#include "lock.h"
#include <atomic>

/**
 * @brief A fair (FIFO) ticket lock implementation.
 *
 * Under high contention, a standard pthread_mutex can starve individual
 * waiters because the OS scheduler and the mutex implementation may
 * allow a recent releaser to re-acquire immediately.  A ticket lock
 * serialises all waiters in strict arrival order, guaranteeing that
 * every thread that calls acquire() will eventually be granted the lock.
 *
 * This is used specifically for the page-fault lock where 16 cores
 * contend and starvation of a single core can freeze the entire
 * barrier-synchronised simulation.
 */
class TicketLock : public LockImplementation
{
public:
   TicketLock()
      : _next_ticket(0)
      , _now_serving(0)
   {}

   ~TicketLock() override {}

   void acquire() override
   {
      // Atomically grab a ticket number
      uint64_t my_ticket = _next_ticket.fetch_add(1, std::memory_order_relaxed);
      // Spin until our ticket is being served
      while (_now_serving.load(std::memory_order_acquire) != my_ticket)
      {
         // Yield to avoid burning CPU in a tight spin
         cpuRelax();
      }
   }

   void release() override
   {
      // Advance to the next ticket, releasing the next waiter in FIFO order
      _now_serving.fetch_add(1, std::memory_order_release);
   }

private:
   // A spin-wait hint: x86's pause, AArch64's yield; elsewhere (RISC-V, whose
   // pause needs Zihintpause) a compiler barrier, so the loop still re-reads
   static inline void cpuRelax()
   {
#if defined(__x86_64__) || defined(__i386__)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield" ::: "memory");
#else
      asm volatile("" ::: "memory");
#endif
   }

   std::atomic<uint64_t> _next_ticket;
   std::atomic<uint64_t> _now_serving;
};

#endif // TICKET_LOCK_H
