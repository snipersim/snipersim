/* Pac-Sim in the QEMU plugin: the adapter between the plugin and the
 * front-end-independent sampler in sift/pacsim_sampler.h.
 *
 * The plugin feeds it every translated block's execution, loop back-edges,
 * synchronisation-point entries, the ROI's begin and end, and the exit; it
 * asks the plugin, through Hooks, whether a vCPU's channel is alive, for a
 * magic command's answer, for a recording-mode switch and for a flush of a
 * vCPU's pending count.  Options arrive as plugin arguments:
 *
 *   pacsim                    enable (needs 'response': the sampler asks questions)
 *   pacsim-dir=<dir>          where pacsim_regions.json goes (default .)
 *   pacsim-region-size=<K>    instructions between the threads before a back-edge cuts (50000)
 *   pacsim-min-region-size=<K>
 *   pacsim-cluster-threshold=<f>
 *   pacsim-min-measurements=<n>
 *   pacsim-warmup=0|1         cache warm-up (1) or instruction counting between measured regions
 *   pacsim-warmup-window=<K>  warm only the last K thousand instructions of a fast-forwarded region (0: all of it)
 *   pacsim-converge=<f>       fast-forward a cluster only once its last two measurements agree to within f (0.10)
 *   pacsim-syms=<elf>         the guest's ELF, for the synchronisation points' addresses
 *                             (pthread_barrier_wait, GOMP_*, _omp_fn*, MPI_Barrier); without
 *                             it regions come from loop back-edges only
 *   pacsim-sync=<name>[,..]   more synchronisation routines, by symbol name: a program's
 *                             own barrier, say (found in the ELF like the others)
 *   pacsim-loop-min=<n>       shortest loop iteration (instructions) whose back-edge may end a
 *                             region: 1 = any back-edge (default, the paper's rule), 0 = 1% of
 *                             the region size
 *   pacsim-markers=cond|all   backward branches that may end a region: conditional only
 *                             (default, the paper's; with pacsim-loop-min=1 the paper's rule
 *                             exactly) or unconditional jumps too */
#ifndef PACSIM_QEMU_H
#define PACSIM_QEMU_H

#include <stdint.h>
#include <functional>
#include <string>

namespace pacsim_qemu
{
   struct Hooks
   {
      std::function<bool()> in_roi;
      std::function<bool(unsigned)> channel_alive;
      std::function<uint64_t(unsigned, uint64_t, uint64_t)> magic;
      std::function<void(int)> set_mode;          /* 0 detailed, 1 memory-only, 2 instruction counting */
      std::function<void(unsigned)> flush_counts;
   };

   /* a plugin argument; true when it was one of ours */
   bool option(const std::string &key, const std::string &val);
   bool enabled();
   void activate(const Hooks &hooks, unsigned max_vcpus, bool verbose);
   void announce();

   void thread_start(unsigned cpu);
   /* The events.  With can_block false an event that would wait for Sniper
      is left undone and reported (true); the plugin then calls again, where
      it can wait, with can_block true.  See pacsim::Sampler. */
   bool block(unsigned cpu, uint32_t n, uint64_t vaddr, bool can_block = true);
   bool back_edge(unsigned cpu, uint64_t vaddr, bool conditional, bool can_block = true);
   bool sync_point(unsigned cpu, uint64_t site, bool can_block = true);
   bool is_sync_entry(uint64_t vaddr);
   void roi_begin(unsigned cpu);
   void roi_end(unsigned cpu);          /* measures the last region, writes the file */
   void at_exit();
}

#endif
