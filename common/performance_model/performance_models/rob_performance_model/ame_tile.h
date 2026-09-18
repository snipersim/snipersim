/* The attached matrix tile: where AME instructions execute when the unit is
 * not a functional unit of the invoking core.
 *
 * The core's ROB still holds every AME micro-op (so the M and accumulator
 * registers give real dependencies and the window fills when the tile is
 * slow), but the op's execution time comes from here: an in-order tile with
 * its own throughput (squares per cycle, bytes per cycle) and its own memory
 * port, connected at the cache level the coupling names.  In ATX's terms:
 *
 *   ica      no tile -- the AME classes are costed in the core's pipeline
 *            from perf_model/core/riscv/ame_* (the IME-equivalent baseline)
 *   oca_l1   the tile's port enters at the core's L1D
 *   oca_l2   ... at the L2 (shared with the core when the cache config
 *            says so: an out-of-core accelerator)
 *   oca_llc  ... at the last-level cache
 *   nca      at the last-level cache, with a transfer engine that fetches a
 *            square's rows when the instruction is dispatched, ahead of the
 *            tile executing it (ATX's near-core accelerator with its UTE)
 *
 * One tile per core; the ownership the extension defines (ame.acquire) is
 * not arbitrated between cores yet.  Statistics under ame_tile.<core>. */
#ifndef AME_TILE_H
#define AME_TILE_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "mem_component.h"
#include "hit_where.h"
#include "core.h"
#include <unordered_map>
#include <vector>

class Core;
class DynamicMicroOp;
class ComponentTime;

class AmeTile
{
public:
   /* NULL when the coupling is ica (nothing attached). */
   static AmeTile *create(Core *core);

   bool owns(const DynamicMicroOp &uop) const;

   /* The op enters the core's window: the transfer engine (nca) starts
      fetching its rows. */
   void dispatch(const DynamicMicroOp &uop, SubsecondTime now);

   /* The core issues the op to the tile.  Returns the cycles until its
      result is available to the core, and where its rows hit. */
   uint64_t issue(DynamicMicroOp &uop, const ComponentTime &now, HitWhere::where_t &hit_where);

private:
   AmeTile(Core *core, MemComponent::component_t level, bool prefetch);

   Core *m_core;
   MemComponent::component_t m_level;
   bool m_prefetch;
   uint64_t m_dispatch_latency, m_result_latency, m_bytes_per_cycle, m_mma_issue, m_mma_latency, m_other_latency;
   SubsecondTime m_free;                                      /* when the tile accepts its next op */
   std::unordered_map<IntPtr, SubsecondTime> m_prefetched;    /* row address -> arrival (nca) */

   UInt64 m_ops, m_rows, m_bytes, m_mma, m_prefetch_hits;
   SubsecondTime m_busy, m_queue_wait, m_idle, m_mem_latency;

   /* The DMA-attached accelerator (Xsnacc acc.task / acc.wait), sharing the
      tile's datapath and port: a task's input blocks stream in through the
      port at dispatch of acc.task's loads, the compute runs at the tile's
      multiply-accumulate rate, and the output block streams out; the core
      only waits at acc.wait.  Work per task is derived from the operand
      sizes (X: T x nin, W: nout x nin, Y: T x nout floats). */
   uint64_t m_macs_per_cycle, m_line_bytes;
   SubsecondTime m_task_in_done;                   /* when the current task's inputs are in */
   std::vector<UInt32> m_task_in_sizes;            /* the current task's input blocks (any order) */
   SubsecondTime m_task_done;                      /* when the last task's output is out */
   UInt64 m_tasks, m_task_bytes_in, m_task_bytes_out, m_task_macs;
   SubsecondTime m_task_dma, m_task_compute, m_task_wait;
   SubsecondTime stream(Core::mem_op_t op, IntPtr addr, UInt32 size, IntPtr eip, SubsecondTime start, SubsecondTime &busy);
};

#endif
