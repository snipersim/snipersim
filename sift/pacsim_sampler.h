/* Pac-Sim's sampler, independent of the front end that drives it.
 *
 * Liu, Sabu, Chaudhari, Kang, Carlson, "Pac-Sim: Simulation of
 * multi-threaded workloads using intelligent, live sampling", TACO 2024.
 *
 * The front end -- the Pin recorder, the QEMU plugin -- counts every basic
 * block of every thread into the sampler, tells it about loop back-edges and
 * synchronisation points, and the ROI's begin and end.  The sampler cuts the
 * run into regions there, clusters each finished region by its block mix,
 * predicts the next region's cluster from each cut point's history, and asks
 * the front end to switch the simulator between detailed simulation (a
 * region that is measured) and warm-up or instruction counting (one that is
 * extrapolated from the latest measurement of its cluster).  At ROI end it
 * measures the last region, extrapolates the rest, and writes
 * <dir>/pacsim_regions.json for tools/pacsim.
 *
 * What it needs from the front end is the FrontEnd interface below: whether
 * the ROI is open and a thread's channel alive, the simulator's answers to
 * magic commands, the mode switch, a flush of a thread's pending instruction
 * count, a lock nobody blocks on, and a place to print. */
#ifndef PACSIM_SAMPLER_H
#define PACSIM_SAMPLER_H

#include "bbv_count.h"

#include <stdint.h>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace pacsim
{

enum class Mode { Detailed, FastForward };
/* the region types tools/pacsim reads (1 and 2 are barrier points, the rest loop points) */
enum class RecordType : int { PointUndef = 0, MultiThread = 1, SingleThread = 2, IntraBarrier = 3, EndPgm = 7 };
/* where a region's time came from (tools/pacsim: 1 = measured) */
enum class Level : int { Unknown = 0, FromSelf = 1, FromLocal = 2, FromGlobal = 3, FromFastForward = 4, FromOtherCluster = 5, FromGlobalAvg = 6 };
/* what was predicted and what happened: (F)ast-forward or (D)etailed, before and after */
enum class Predict : int { F2F = 0, F2D = 1, D2F = 2, D2D = 3 };
/* what the front end records between measured regions */
enum class FastForwardKind { Warmup, Icount };

struct Options
{
   uint64_t region_size = 50000000;      /* instructions between the threads before a loop back-edge cuts */
   uint64_t min_region_size = 2000000;   /* a shorter region joins the next */
   /* Which back-edges may cut.  The paper (Liu et al., TACO 2024, III-A)
      takes conditional branches with negative offsets as loop markers and
      cuts at the first one beyond the budget: the default, loop_min = 1 with
      conditional_only.  Two variations are available, tried while looking
      into small budgets (regions a fraction of a loop iteration, where which
      loop the cut falls in matters), neither of which proved better than the
      paper's rule on the suite's loop shape: loop_min > 1 admits a back-edge
      only when its loop's previous iteration ran at least that many
      instructions (0: 1% of the budget), with any back-edge accepted once
      twice the budget has passed; conditional_only = false admits
      unconditional backward jumps too (a compiler closes many loops with
      one). */
   uint64_t loop_min = 1;
   bool conditional_only = true;
   double   cluster_threshold = 0.2;     /* L1 distance between block mixes below which regions share a cluster */
   unsigned min_measurements = 2;        /* measure a cluster this many times before fast-forwarding it */
   /* With cache warm-up between measured regions (fast_forward == Warmup):
      how much of a fast-forwarded region is warmed, counted back from its
      expected end (the budget).  0 warms the whole region, which costs
      about 0.6 of detailed simulation per instruction (the caches are
      simulated functionally) and caps the speedup near 1.5x however much
      repeats; a window counts through the rest of the region at almost no
      cost and warms only the stretch before the next region, where the
      caches it leaves behind matter. */
   uint64_t warmup_window = 0;
   /* A cluster is fast-forwarded only once its last two measurements agree
      on the time per instruction to within this fraction (0: as soon as it
      has min_measurements).  The first regions of a run carry its cold
      start -- the agent's first prompt ran at 0.99 fs per instruction, its
      later prompts at 0.55 -- and a representative taken then overstates
      every region extrapolated from it. */
   double converge = 0.10;
   FastForwardKind fast_forward = FastForwardKind::Warmup;
   unsigned max_threads = 128;
   std::string output_dir = ".";
   bool verbose = false;
};

/* What the front end provides.  Every call may come from any thread. */
class FrontEnd
{
public:
   virtual ~FrontEnd() {}
   virtual bool inRoi() = 0;
   virtual bool channelAlive(unsigned tid) = 0;
   /* a magic command through the thread's own channel; 0 when it cannot be asked */
   virtual uint64_t magic(unsigned tid, uint64_t cmd, uint64_t arg) = 0;
   /* the front end's own recording mode: detailed, or the fast-forward kind chosen */
   virtual void setDetailed() = 0;
   virtual void setFastForward(FastForwardKind kind) = 0;
   /* send the thread's pending instruction count now, so that a mode switch
      does not carry counts across; a no-op when there is none */
   virtual void flushCounts(unsigned tid) = 0;
   /* a lock nobody blocks on: the holder may wait for the simulator's answer,
      which needs every other thread to keep delivering instructions */
   virtual bool tryLock() = 0;
   virtual void lock() = 0;
   virtual void unlock() = 0;
   virtual void log(const std::string &line) = 0;
};

struct Region
{
   uint64_t fs = 0, l2 = 0, l2miss = 0, max_ins = 0;
   std::vector<uint64_t> ins;          /* per thread */
   std::vector<double> bbv;            /* per thread, normalised (written to the file) */
   unsigned nactive = 0;               /* threads that ran a meaningful share of the region */
   std::vector<double> mix;            /* the region's block mix summed over its active threads, unit-normalised: what it is clustered by */
   uint64_t cluster = 0;
   uint64_t pc = 0;
   RecordType type = RecordType::IntraBarrier;
   Level level = Level::Unknown;
   Predict predict = Predict::D2D;
   bool solved = false;
   double time_per_ins = 0;
   double prev_time_per_ins = 0;       /* the measurement before the latest, for Options::converge */
   unsigned measurements = 0;          /* of a cluster's representative: how many times it was measured */
   uint64_t lag = 0;                   /* at the region's start: how far the cutting thread's core was behind the furthest core */
   uint64_t region_fs = 0;             /* the region's own time, measured or from its cluster, before Sampler::account() made fs its share of the run's (written as region_fs) */
   /* the measurement an unmeasured region's time came from (repair()), for
      the per-thread account (Sampler::account()) */
   bool has_rep = false;
   uint64_t rep_fs = 0;
   std::vector<uint64_t> rep_ins;
   std::vector<double> rep_bbv;

   void measured(uint64_t fs_, uint64_t l2_, uint64_t l2miss_);
   void absorb(const Region &r);       /* the latest measurement stands for the cluster */
   bool settled(unsigned min_measurements, double converge) const;
   void extrapolate(double tpi, Level from);
   void extrapolate(const Region &rep, Level from);
   /* a thread's time in this region at the rate of the representative's thread most like it */
   double threadTime(unsigned t, unsigned nthreads) const;
   std::string json(unsigned nthreads) const;
};

class Clusterer
{
   struct Centroid { std::vector<double> bbv; unsigned nactive; uint64_t id; bool measured; };
   std::vector<Centroid> m_centroids;
   double m_threshold;
   uint64_t m_next = 0;
public:
   Clusterer(double threshold) : m_threshold(threshold) {}
   uint64_t assign(const std::vector<double> &bbv, unsigned nactive, bool measured);
};

class History
{
   static const unsigned MAXDEPTH = 16;
   std::map<std::vector<uint64_t>, std::map<uint64_t, unsigned> > m_next;
public:
   void learn(const std::vector<uint64_t> &hist);
   bool predict(const std::vector<uint64_t> &hist, uint64_t &out) const;
};

struct Predictor
{
   std::map<uint64_t, Region> known;   /* cluster -> measured representative */
   History history;
   uint64_t last_cluster = 0;
   bool any = false;
};

class Sampler
{
public:
   Sampler(const Options &options, FrontEnd &front_end);
   ~Sampler();

   /* The three events, each of which may end in a question to the simulator
      (a cut, or a flush of the thread's count) that waits for the answer.  A
      front end that cannot wait where the event fires -- the QEMU plugin,
      whose vCPU must leave the run loop first -- calls with can_block false:
      an event that would wait is then left undone and reported (true), and
      the front end calls again, where it can wait, with can_block true.  An
      event that needs no answer completes either way (false). */
   /* every basic block, every thread, every mode */
   bool block(unsigned tid, uint32_t n, uint64_t addr, bool can_block = true);
   /* the ROI opened, on this thread, in detailed mode */
   void roiBegin(unsigned tid);
   /* a loop back-edge, on any thread: a cut point once the budget is spent */
   bool backEdge(unsigned tid, uint64_t addr, bool conditional, bool can_block = true);
   /* a synchronisation point, keyed by its call site: a cut once every active thread has reached it */
   bool syncPoint(unsigned tid, uint64_t addr, bool can_block = true);
   void threadStart(unsigned tid);
   /* the end: at ROI end (the last region can still be measured), at exit,
      or at the front end's own end (the channels are closed by then) */
   void finish(unsigned tid, uint64_t pc, bool can_measure);

   const Options &options() const { return m_options; }

private:
   uint64_t magic(unsigned tid, uint64_t cmd, uint64_t arg = 0);
   void flushIcount(unsigned tid);
   void snapshot(unsigned tid);
   void gotoDetailed(unsigned tid);
   void gotoFastForward(unsigned tid);
   uint64_t sinceCut() const;
   bool warmupDue(uint64_t extra) const;
   uint64_t activeShare() const;
   unsigned activeThreads() const;
   void markRegionStart(uint64_t pc);
   void start(unsigned tid, uint64_t pc);
   void endRegion(unsigned tid, uint64_t pc, RecordType type, bool can_measure);
   Mode decide(uint64_t pc);
   void cut(unsigned tid, uint64_t pc, RecordType type, bool can_measure = true);
   void repair();
   void account();
   void write();

   const Options m_options;
   FrontEnd &m_fe;
   const unsigned m_nt;
   const uint64_t m_threshold, m_min;
   const uint64_t m_loop_min;                         /* a loop iteration this long makes its back-edge a cut candidate (Options::loop_min) */
   Clusterer m_clusterer;
   Mode m_mode = Mode::Detailed;
   bool m_started = false, m_finished = false;
   bool m_warming = false;                /* a windowed warm-up has begun in this fast-forwarded region */
   std::vector<uint64_t> m_ins, m_region_ins;        /* per thread: total, at the region's start */
   std::vector<Bbv *> m_bbv;
   std::vector<std::vector<uint64_t> > m_region_bbv;  /* per thread, at the region's start */
   std::vector<bool> m_flush;                         /* a count flush is owed before the next record */
   std::vector<std::unordered_map<uint64_t, uint64_t> > m_last_edge;   /* per thread: the instruction count when a back-edge last fired */
   unsigned m_nthreads = 1;
   uint64_t m_prior_fs = 0, m_prior_l2 = 0, m_prior_l2miss = 0;
   uint64_t m_region_pc = 0;
   uint64_t m_region_lag = 0;
   std::vector<Region> m_regions;
   std::vector<uint64_t> m_cluster_history;
   std::map<uint64_t, Predictor> m_predictors;
   History m_global_history;              /* the run's cluster sequence, for a cut point not seen before */
   /* Per synchronisation site: how many times each thread has reached it
      since the region began, and per thread the site of an arrival whose cut
      was left to the front end's second call (not counted again there).
      Under m_arrival_lock, a spin lock held for the bookkeeping only: the
      main lock is held across the simulator's answers, and an arrival that
      could not take it was dropped (syncPoint). */
   std::map<uint64_t, std::vector<unsigned> > m_arrived;
   std::vector<uint64_t> m_sync_deferred;
   std::atomic_flag m_arrival_lock = ATOMIC_FLAG_INIT;
   void arrivalLock() { while (m_arrival_lock.test_and_set(std::memory_order_acquire)) ; }
   void arrivalUnlock() { m_arrival_lock.clear(std::memory_order_release); }
   bool allArrived(uint64_t addr, unsigned tid);
   std::map<uint64_t, Region> m_global;                 /* cluster -> measured representative, any cut point */
};

}

#endif
