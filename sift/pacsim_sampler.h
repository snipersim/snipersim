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
#include <map>
#include <set>
#include <string>
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
   double   cluster_threshold = 0.2;     /* L1 distance between block mixes below which regions share a cluster */
   unsigned min_measurements = 2;        /* measure a cluster this many times before fast-forwarding it */
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
   unsigned measurements = 0;          /* of a cluster's representative: how many times it was measured */
   uint64_t lag = 0;                   /* at the region's start: how far the cutting thread's core was behind the furthest core */

   void measured(uint64_t fs_, uint64_t l2_, uint64_t l2miss_);
   void absorb(const Region &r);       /* the latest measurement stands for the cluster */
   void extrapolate(double tpi, Level from);
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

   /* every basic block, every thread, every mode */
   void block(unsigned tid, uint32_t n, uint64_t addr);
   /* the ROI opened, on this thread, in detailed mode */
   void roiBegin(unsigned tid);
   /* a loop back-edge, on any thread: a cut point once the budget is spent */
   void backEdge(unsigned tid, uint64_t addr);
   /* a synchronisation point, keyed by its call site: a cut once every active thread has reached it */
   void syncPoint(unsigned tid, uint64_t addr);
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
   unsigned activeThreads() const;
   void markRegionStart(uint64_t pc);
   void start(unsigned tid, uint64_t pc);
   void endRegion(unsigned tid, uint64_t pc, RecordType type, bool can_measure);
   Mode decide(uint64_t pc);
   void cut(unsigned tid, uint64_t pc, RecordType type, bool can_measure = true);
   void repair();
   void write();

   const Options m_options;
   FrontEnd &m_fe;
   const unsigned m_nt;
   const uint64_t m_threshold, m_min;
   Clusterer m_clusterer;
   Mode m_mode = Mode::Detailed;
   bool m_started = false, m_finished = false;
   std::vector<uint64_t> m_ins, m_region_ins;        /* per thread: total, at the region's start */
   std::vector<Bbv *> m_bbv;
   std::vector<std::vector<uint64_t> > m_region_bbv;  /* per thread, at the region's start */
   std::vector<bool> m_flush;                         /* a count flush is owed before the next record */
   unsigned m_nthreads = 1;
   uint64_t m_prior_fs = 0, m_prior_l2 = 0, m_prior_l2miss = 0;
   uint64_t m_region_pc = 0;
   uint64_t m_region_lag = 0;
   std::vector<Region> m_regions;
   std::vector<uint64_t> m_cluster_history;
   std::map<uint64_t, Predictor> m_predictors;
   std::map<uint64_t, std::set<unsigned> > m_arrived;  /* per synchronisation site: the threads that reached it since the region began */
   std::map<uint64_t, Region> m_global;                 /* cluster -> measured representative, any cut point */
};

}

#endif
