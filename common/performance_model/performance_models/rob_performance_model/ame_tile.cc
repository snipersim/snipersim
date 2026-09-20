#include "ame_tile.h"
#include "core.h"
#include "dynamic_micro_op.h"
#include "micro_op.h"
#include "instruction.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include "riscv_isa.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

AmeTile *AmeTile::create(Core *core)
{
   /* Only the RISC-V configurations name a coupling; every other configuration
    * has no attached tile (ica: the matrix classes cost on the core). */
   const String coupling = Sim()->getCfg()->hasKey("perf_model/ame_tile/coupling") ? Sim()->getCfg()->getString("perf_model/ame_tile/coupling") : String("ica");
   if (coupling == "ica")
      return NULL;
   const MemComponent::component_t llc = (MemComponent::component_t)(Sim()->getCfg()->getInt("perf_model/cache/levels") - 2 + MemComponent::L2_CACHE);
   MemComponent::component_t level;
   bool prefetch = false;
   if (coupling == "oca_l1")       level = MemComponent::L1_DCACHE;
   else if (coupling == "oca_l2")  level = MemComponent::L2_CACHE;
   else if (coupling == "oca_llc") level = llc;
   else if (coupling == "nca")   { level = llc; prefetch = true; }
   else LOG_ASSERT_ERROR(false, "perf_model/ame_tile/coupling must be ica, oca_l1, oca_l2, oca_llc or nca (got %s)", coupling.c_str());
   LOG_ASSERT_ERROR(level <= llc, "perf_model/ame_tile/coupling names a cache level the configuration does not have");
   return new AmeTile(core, level, prefetch);
}

AmeTile::AmeTile(Core *core, MemComponent::component_t level, bool prefetch)
   : m_core(core), m_level(level), m_prefetch(prefetch)
   , m_dispatch_latency(Sim()->getCfg()->getIntDefault("perf_model/ame_tile/dispatch_latency", 4))
   , m_result_latency(Sim()->getCfg()->getIntDefault("perf_model/ame_tile/result_latency", 4))
   , m_bytes_per_cycle(Sim()->getCfg()->getIntDefault("perf_model/ame_tile/bytes_per_cycle", 64))
   , m_mma_issue(Sim()->getCfg()->getIntDefault("perf_model/ame_tile/mma_issue_cycles", 0))
   , m_mma_latency(Sim()->getCfg()->getIntDefault("perf_model/ame_tile/mma_latency", 8))
   , m_other_latency(Sim()->getCfg()->getIntDefault("perf_model/ame_tile/other_latency", 1))
   , m_mma_macs_per_cycle(Sim()->getCfg()->getIntDefault("perf_model/ame_tile/mma_macs_per_cycle", 0))
   , m_n(4)
   , m_free(SubsecondTime::Zero())
   , m_ops(0), m_rows(0), m_bytes(0), m_mma(0), m_prefetch_hits(0)
   , m_busy(SubsecondTime::Zero()), m_queue_wait(SubsecondTime::Zero()), m_idle(SubsecondTime::Zero()), m_mem_latency(SubsecondTime::Zero())
   , m_macs_per_cycle(Sim()->getCfg()->getIntDefault("perf_model/ame_tile/macs_per_cycle", 16))
   , m_line_bytes(Sim()->getCfg()->getIntDefault("perf_model/l1_dcache/cache_block_size", 64))
   , m_task_in_done(SubsecondTime::Zero()), m_task_done(SubsecondTime::Zero())
   , m_tasks(0), m_task_bytes_in(0), m_task_bytes_out(0), m_task_macs(0)
   , m_task_dma(SubsecondTime::Zero()), m_task_compute(SubsecondTime::Zero()), m_task_wait(SubsecondTime::Zero())
{
   registerStatsMetric("ame_tile", core->getId(), "tasks", &m_tasks);
   registerStatsMetric("ame_tile", core->getId(), "task_bytes_in", &m_task_bytes_in);
   registerStatsMetric("ame_tile", core->getId(), "task_bytes_out", &m_task_bytes_out);
   registerStatsMetric("ame_tile", core->getId(), "task_macs", &m_task_macs);
   registerStatsMetric("ame_tile", core->getId(), "task_dma", &m_task_dma);
   registerStatsMetric("ame_tile", core->getId(), "task_compute", &m_task_compute);
   registerStatsMetric("ame_tile", core->getId(), "task_wait", &m_task_wait);
   registerStatsMetric("ame_tile", core->getId(), "ops", &m_ops);
   registerStatsMetric("ame_tile", core->getId(), "rows", &m_rows);
   registerStatsMetric("ame_tile", core->getId(), "bytes", &m_bytes);
   registerStatsMetric("ame_tile", core->getId(), "mma", &m_mma);
   registerStatsMetric("ame_tile", core->getId(), "prefetch_hits", &m_prefetch_hits);
   registerStatsMetric("ame_tile", core->getId(), "busy", &m_busy);
   registerStatsMetric("ame_tile", core->getId(), "queue_wait", &m_queue_wait);
   registerStatsMetric("ame_tile", core->getId(), "idle", &m_idle);
   registerStatsMetric("ame_tile", core->getId(), "mem_latency", &m_mem_latency);
}

static inline const char *class_name(const MicroOp *m)
{
   const unsigned op = m->getInstructionOpcode();
   return op ? rvisa::info(rvisa::class_of(op)).name : "";
}
static inline bool is_ame_class(const MicroOp *m) { return strncmp(class_name(m), "ame_", 4) == 0; }
static inline bool is_acc_class(const MicroOp *m) { return strncmp(class_name(m), "acc_", 4) == 0; }

/* Stream a block through the port line by line: the lines pipeline, so the
   time is the last line's issue plus the longest latency; the port is busy
   for the bytes. */
SubsecondTime AmeTile::stream(Core::mem_op_t op, IntPtr addr, UInt32 size, IntPtr eip, SubsecondTime start, SubsecondTime &busy)
{
   const ComponentPeriod *period = m_core->getDvfsDomain();
   const SubsecondTime cycle = SubsecondTime::SEC() / period->getPeriodInFreqMHz() / 1000000;
   SubsecondTime longest = SubsecondTime::Zero();
   const IntPtr first = addr & ~(IntPtr)(m_line_bytes - 1), last = (addr + size - 1) & ~(IntPtr)(m_line_bytes - 1);
   uint64_t lines = 0;
   for (IntPtr a = first; a <= last; a += m_line_bytes, lines++)
   {
      const uint64_t line_cycles = (m_line_bytes + m_bytes_per_cycle - 1) / m_bytes_per_cycle;
      MemoryResult res = m_core->accessMemoryAt(m_level, op, a, m_line_bytes, eip, start + lines * line_cycles * cycle);
      if (res.latency > longest) longest = res.latency;
   }
   busy = lines * ((m_line_bytes + m_bytes_per_cycle - 1) / m_bytes_per_cycle) * cycle;
   return start + busy + (op == Core::READ ? longest : SubsecondTime::Zero());
}

bool AmeTile::owns(const DynamicMicroOp &uop) const
{
   return is_ame_class(uop.getMicroOp()) || is_acc_class(uop.getMicroOp());
}

bool AmeTile::holds_until_done(const DynamicMicroOp &uop) const
{
   return uop.getMicroOp()->isStore() && strcmp(class_name(uop.getMicroOp()), "acc_wait") == 0;
}

void AmeTile::dispatch(const DynamicMicroOp &uop, SubsecondTime now)
{
   if (!m_prefetch || !uop.getMicroOp()->isLoad() || !is_ame_class(uop.getMicroOp()))
      return;
   /* The transfer engine fetches the row now, through the tile's port. */
   const IntPtr addr = uop.getAddress().address;
   const UInt32 size = uop.getAddress().size ? uop.getAddress().size : uop.getMicroOp()->getMemoryAccessSize();
   MemoryResult res = m_core->accessMemoryAt(m_level, Core::READ, addr, size,
                                             uop.getMicroOp()->getInstruction() ? uop.getMicroOp()->getInstruction()->getAddress() : 0, now);
   m_prefetched[addr] = now + res.latency;
}

uint64_t AmeTile::issue(DynamicMicroOp &uop, const ComponentTime &now, HitWhere::where_t &hit_where)
{
   const MicroOp *m = uop.getMicroOp();
   const SubsecondTime t_now = now.getElapsedTime();
   const SubsecondTime arrival = t_now + m_dispatch_latency * now.getPeriod();
   const SubsecondTime start = arrival > m_free ? arrival : m_free;
   if (start > m_free) m_idle += start - m_free;
   else m_queue_wait += m_free - arrival;

   SubsecondTime busy = now.getPeriod();
   SubsecondTime done;
   hit_where = HitWhere::UNKNOWN;
   const uint32_t flags = rvisa::flags_of(m->getInstructionOpcode());
   m_ops++;

   if (is_acc_class(m))
   {
      /* The DMA-attached accelerator.  acc.task: the core hands over the
         descriptor and continues (its uops cost the dispatch); the inputs
         stream in behind the previous task.  acc.wait: its store uop is the
         output block; the task computes once its inputs are in, streams Y
         out, and the wait's latency is whatever remains. */
      const bool task = strcmp(class_name(m), "acc_task") == 0;
      const IntPtr eip = m->getInstruction() ? m->getInstruction()->getAddress() : 0;
      if (task && m->isLoad())
      {
         const IntPtr addr = uop.getAddress().address;
         const UInt32 size = uop.getAddress().size ? uop.getAddress().size : m->getMemoryAccessSize();
         SubsecondTime b;
         const SubsecondTime s0 = start > m_task_done ? start : m_task_done;
         const SubsecondTime in_done = stream(Core::READ, addr, size, eip, s0, b);
         if (in_done > m_task_in_done) m_task_in_done = in_done;
         m_task_dma += b;
         m_task_bytes_in += size;
         m_task_in_sizes.push_back(size);
         hit_where = (HitWhere::where_t)m_level;
         m_free = start + now.getPeriod();
         return m_dispatch_latency;
      }
      if (task)
      {
         if (m_task_in_sizes.empty()) m_task_in_done = start;
         return m_dispatch_latency;
      }
      /* acc.wait */
      if (m->isStore())
      {
         const IntPtr addr = uop.getAddress().address;
         const UInt32 size = uop.getAddress().size ? uop.getAddress().size : m->getMemoryAccessSize();
         /* The trace lists a task's blocks in address order and merges
            adjacent ones, so the blocks are identified by size alone: the
            descriptor is the small one (<= 64 bytes), the weights the largest
            (nout x nin floats, possibly merged with the nout-float bias that
            follows it in memory), the activations T x nin, the output
            T x nout.  With X/Y = nin/nout the weights block fixes nout, with
            or without the bias, and the assignment that comes out integral
            is the one; MACs = T * nin * nout. */
         uint64_t macs = 0;
         {
            std::vector<UInt32> in;
            for (size_t i = 0; i < m_task_in_sizes.size(); i++)
               if (m_task_in_sizes[i] > 64) in.push_back(m_task_in_sizes[i]);
            const uint64_t yf = size / 4;
            if (in.size() >= 2 && yf)
            {
               size_t wi = 0;
               for (size_t i = 1; i < in.size(); i++) if (in[i] > in[wi]) wi = i;
               const long double wf = in[wi] / 4;
               for (size_t i = 0; i < in.size() && !macs; i++)
               {
                  if (i == wi) continue;
                  const long double xf = in[i] / 4, r = xf / yf;                 /* nin / nout */
                  const long double cands[2] = { sqrtl(wf / r),                                    /* weights alone */
                                                 (-1.0L + sqrtl(1.0L + 4.0L * wf * r)) / (2.0L * r) }; /* weights + bias */
                  for (int h = 0; h < 2 && !macs; h++)
                  {
                     const uint64_t nout = (uint64_t)(cands[h] + 0.5L);
                     if (!nout) continue;
                     const uint64_t nin = (uint64_t)(nout * r + 0.5L);
                     const uint64_t wexp = h == 0 ? nout * nin : nout * (nin + 1);
                     if (nin && wexp == (uint64_t)wf && (uint64_t)xf % nin == 0 && (uint64_t)xf / nin * nout == yf)
                        macs = (uint64_t)xf / nin * nin * nout;
                  }
               }
            }
         }
         const SubsecondTime compute = (macs + m_macs_per_cycle - 1) / m_macs_per_cycle * now.getPeriod();
         SubsecondTime b;
         const SubsecondTime out_done = stream(Core::WRITE, addr, size, eip, m_task_in_done + compute, b);
         m_task_done = out_done;
         m_free = out_done;
         m_task_compute += compute;
         m_task_dma += b;
         m_task_bytes_out += size;
         m_task_macs += macs;
         m_tasks++;
         m_task_in_sizes.clear();
         hit_where = (HitWhere::where_t)m_level;
         const SubsecondTime avail = m_task_done + m_result_latency * now.getPeriod();
         const SubsecondTime wait = avail > t_now ? avail - t_now : now.getPeriod();
         m_task_wait += wait;
         return SubsecondTime::divideRounded(wait, now.getPeriod());
      }
      return 1;
   }

   if (m->isLoad() || m->isStore())
   {
      const IntPtr addr = uop.getAddress().address;
      const UInt32 size = uop.getAddress().size ? uop.getAddress().size : m->getMemoryAccessSize();
      busy = ((size + m_bytes_per_cycle - 1) / m_bytes_per_cycle) * now.getPeriod();
      SubsecondTime mem = SubsecondTime::Zero();
      std::unordered_map<IntPtr, SubsecondTime>::iterator pf = m_prefetched.find(addr);
      if (m->isLoad() && pf != m_prefetched.end())
      {
         /* fetched by the transfer engine at dispatch: whatever is still outstanding */
         mem = pf->second > start ? pf->second - start : SubsecondTime::Zero();
         hit_where = (HitWhere::where_t)m_level;
         m_prefetched.erase(pf);
         m_prefetch_hits++;
      }
      else
      {
         MemoryResult res = m_core->accessMemoryAt(m_level, m->isLoad() ? Core::READ : Core::WRITE, addr, size,
                                                   m->getInstruction() ? m->getInstruction()->getAddress() : 0, start);
         mem = res.latency;
         hit_where = res.hit_where;
      }
      m_rows++;
      m_bytes += size;
      m_mem_latency += mem;
      if (size >= 8 && (size & (size - 1)) == 0 && size / 4 <= 64)
         m_n = size / 4;                                    /* a row of N 32-bit elements */
      /* loads complete when the data is in; stores leave the tile once their bytes have gone out */
      done = start + busy + (m->isLoad() ? mem : SubsecondTime::Zero());
   }
   else if (flags & rvisa::FLAG_MATRIX)
   {
      /* An N x N x N square product: N^3 multiply-accumulates on an array of
         mma_macs_per_cycle (N*N by default, one square row per cycle), unless
         mma_issue_cycles overrides the interval. */
      const uint64_t macs = m_n * m_n * m_n;
      const uint64_t rate = m_mma_macs_per_cycle ? m_mma_macs_per_cycle : m_n * m_n;
      const uint64_t issue = m_mma_issue ? m_mma_issue : (macs + rate - 1) / rate;
      busy = issue * now.getPeriod();
      done = start + (issue > m_mma_latency ? issue : m_mma_latency) * now.getPeriod();
      m_mma++;
   }
   else
   {
      busy = m_other_latency * now.getPeriod();
      done = start + busy;
   }
   m_free = start + busy;
   m_busy += busy;

   /* An X-register result (agettyp, mgettyp, ame.acquire) travels back to
      the core; results held in the tile do not. */
   const bool x_result = m->getDestinationRegistersLength() > 0 && m->getDestinationRegister(0) < 32;
   if (x_result)
      done += m_result_latency * now.getPeriod();
   if (done < t_now + now.getPeriod())
      done = t_now + now.getPeriod();
   return SubsecondTime::divideRounded(done - t_now, now.getPeriod());
}
