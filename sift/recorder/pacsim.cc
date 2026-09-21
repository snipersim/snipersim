#include "pacsim.h"
#include "globals.h"
#include "threads.h"
#include "recorder_control.h"
#include "bbv_count.h"
#include "../../include/sim_api.h"

#include "pin.H"
#include <cmath>
#include <cstdlib>
#include <unistd.h>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pacsim
{

static const unsigned NT = MAX_NUM_THREADS_DEFAULT;
static const unsigned DIM = Bbv::NUM_BBV;

enum class Mode { Detailed, FastForward };
/* the region types pacsimscripts reads (1 and 2 are barrier points, the rest loop points) */
enum class RecordType : int { PointUndef = 0, MultiThread = 1, SingleThread = 2, IntraBarrier = 3, EndPgm = 7 };
/* where a region's time came from (pacsimscripts: 1 = measured) */
enum class Level : int { Unknown = 0, FromSelf = 1, FromLocal = 2, FromGlobal = 3, FromFastForward = 4, FromOtherCluster = 5, FromGlobalAvg = 6 };
/* what was predicted and what happened: (F)ast-forward or (D)etailed, before and after */
enum class Predict : int { F2F = 0, F2D = 1, D2F = 2, D2D = 3 };

/* ------------------------------------------------------------ one region */
struct Region
{
   uint64_t fs = 0, l2 = 0, l2miss = 0, max_ins = 0;
   std::vector<uint64_t> ins;          /* per thread */
   std::vector<double> bbv;            /* per thread, DIM normalised dimensions (written to the file) */
   unsigned nactive = 0;               /* threads that ran a meaningful share of the region */
   std::vector<double> mix;            /* the region's block mix summed over its active threads, unit-normalised: what it is clustered by.
                                          A thread that lags a little behind the others (it started late, it was descheduled) shows the
                                          previous phase's tail in its own vector; summed over the threads that is a small admixture, and
                                          the region still clusters with the phase it is. */
   uint64_t cluster = 0;
   ADDRINT pc = 0;
   RecordType type = RecordType::IntraBarrier;
   Level level = Level::Unknown;
   Predict predict = Predict::D2D;
   bool solved = false;
   double time_per_ins = 0;
   unsigned measurements = 0;              /* of a cluster's representative: how many times it was measured */
   uint64_t lag = 0;                       /* at the region's start: how far the cutting thread's core was behind the furthest core */

   void measured(uint64_t fs_, uint64_t l2_, uint64_t l2miss_)
   {
      fs = fs_; l2 = l2_; l2miss = l2miss_;
      level = Level::FromSelf; solved = true; measurements = 1;
      time_per_ins = max_ins ? (double)fs / (double)max_ins : 0;
   }
   void absorb(const Region &r) { fs = r.fs; max_ins = r.max_ins; time_per_ins = r.time_per_ins; measurements++; }   /* the latest measurement stands for the cluster */
   /* An unmeasured region: its threads' time at the rate of its cluster,
      less the lag its thread had at the start -- a core that is behind
      moves the run's time only once it has caught up. */
   void extrapolate(double tpi, Level from)
   {
      time_per_ins = tpi;
      const double t = tpi * (double)max_ins;
      fs = t > (double)lag ? (uint64_t)(t - (double)lag) : 0;
      level = from; solved = true;
   }

   std::string json(unsigned nthreads) const
   {
      std::stringstream out;
      out << "  { \"max_ins_num\": " << max_ins << ", \"fs\": " << fs << ", \"l2count\": " << l2 << ", \"l2misscount\": " << l2miss
          << ", \"clusterid\": " << cluster << ", \"pc\": " << pc << ", \"freq\": 0"
          << ", \"predict_result_type\": " << (int)predict << ", \"recordtype\": " << (int)type
          << ", \"feature_level\": " << (int)level << ",\n    \"insnums\": [";
      for (unsigned t = 0; t < nthreads; t++) out << (t ? ", " : "") << (t < ins.size() ? ins[t] : 0);
      out << "],\n    \"bbv\": [";
      for (unsigned i = 0; i < nthreads * DIM; i++) out << (i ? ", " : "") << (i < bbv.size() ? bbv[i] : 0.0);
      out << "] }";
      return out.str();
   }
};

static double l1(const std::vector<double> &a, const std::vector<double> &b)
{
   double d = 0;
   const size_t n = a.size() < b.size() ? a.size() : b.size();
   for (size_t i = 0; i < n; i++) d += std::fabs(a[i] - b[i]);
   return d;
}

/* ------------------------------------------------- clustering by BBV distance
 * A region joins the first cluster whose representative vector is within
 * the threshold (L1 over every thread's normalised BBV; a thread with fewer
 * than a million instructions in the region contributes nothing), measured
 * clusters searched before fast-forwarded ones; otherwise it founds one. */
class Clusterer
{
   struct Centroid { std::vector<double> bbv; unsigned nactive; uint64_t id; bool measured; };
   std::vector<Centroid> m_centroids;
   double m_threshold;
   uint64_t m_next = 0;
public:
   Clusterer(double threshold) : m_threshold(threshold) {}
   /* Regions cluster only with the same number of active threads: the
      same code run by one thread -- a thread catching up with the others,
      which wait for it at the simulator's barrier -- costs a fraction of the
      global time that the same code run by all of them does. */
   uint64_t assign(const std::vector<double> &bbv, unsigned nactive, bool measured)
   {
      for (int pass = 0; pass < 2; pass++)
         for (Centroid &c : m_centroids)
            if (c.nactive == nactive && c.measured == (pass == 0) && l1(c.bbv, bbv) < m_threshold)
            {
               if (measured && !c.measured) c.measured = true;
               return c.id;
            }
      m_centroids.push_back(Centroid{bbv, nactive, m_next, measured});
      return m_next++;
   }
};

/* --------------------------------------- what comes next, from the history
 * The next cluster of a cut point is predicted from the sequence of clusters
 * seen so far: the longest suffix of the history that has occurred before
 * votes with what followed it (the trie of the original). */
class History
{
   static const unsigned MAXDEPTH = 16;
   std::map<std::vector<uint64_t>, std::map<uint64_t, unsigned> > m_next;
public:
   void learn(const std::vector<uint64_t> &hist)
   {
      if (hist.empty()) return;
      const uint64_t last = hist.back();
      std::vector<uint64_t> key;
      for (unsigned d = 1; d <= MAXDEPTH && d < hist.size(); d++)
      {
         key.insert(key.begin(), hist[hist.size() - 1 - d]);
         m_next[key][last]++;
      }
   }
   bool predict(const std::vector<uint64_t> &hist, uint64_t &out) const
   {
      std::vector<uint64_t> key;
      const std::map<uint64_t, unsigned> *best = NULL;
      for (unsigned d = 1; d <= MAXDEPTH && d <= hist.size(); d++)
      {
         key.insert(key.begin(), hist[hist.size() - d]);
         std::map<std::vector<uint64_t>, std::map<uint64_t, unsigned> >::const_iterator it = m_next.find(key);
         if (it == m_next.end()) break;
         best = &it->second;
      }
      if (!best) return false;
      unsigned n = 0;
      for (std::map<uint64_t, unsigned>::const_iterator it = best->begin(); it != best->end(); ++it)
         if (it->second > n) { n = it->second; out = it->first; }
      return n > 0;
   }
};

/* --------------------------------------------- the predictor of one cut point */
struct Predictor
{
   std::map<uint64_t, Region> known;      /* cluster -> measured representative */
   History history;
   uint64_t last_cluster = 0;
   bool any = false;
};

/* -------------------------------------------------------------- the sampler */
class Sampler
{
   const uint64_t m_threshold, m_min;
   const double m_cluster_threshold;
   const bool m_warmup;
   const unsigned m_min_measurements;      /* a cluster is fast-forwarded only after this many measurements: the first instance of a phase carries its warm-up */
   Clusterer m_clusterer;
   Mode m_mode = Mode::Detailed;
   bool m_started = false, m_finished = false;
   std::vector<uint64_t> m_ins, m_region_ins;        /* per thread: total, at the region's start */
   std::vector<Bbv *> m_bbv;
   std::vector<std::vector<uint64_t> > m_region_bbv;  /* per thread, at the region's start */
   std::vector<bool> m_flush;                         /* an icount flush is owed before the next record */
   std::vector<bool> m_active;
   unsigned m_nthreads = 1;
   uint64_t m_prior_fs = 0, m_prior_l2 = 0, m_prior_l2miss = 0;
   ADDRINT m_region_pc = 0;
   uint64_t m_region_lag = 0;
   std::vector<Region> m_regions;
   std::vector<uint64_t> m_cluster_history;
   std::map<ADDRINT, Predictor> m_predictors;
   std::map<ADDRINT, std::set<unsigned> > m_arrived;  /* per synchronisation site: the threads that reached it since the region began */
   std::map<uint64_t, Region> m_global;               /* cluster -> measured representative, any cut point */
   /* Held while a thread cuts: it may wait for the simulator's answer, which
      needs every other thread to keep delivering instructions -- so nobody
      blocks on it; a thread that finds it taken skips its cut. */
   PIN_MUTEX m_mutex;

   /* the simulator's global counters, asked through the calling thread's own
      channel; only while its channel is alive and inside the ROI, since after
      endROI the channel has been ended and a Magic would wait forever */
   uint64_t magic(unsigned tid, uint64_t cmd, uint64_t arg = 0)
   {
      return (in_roi && thread_data[tid].output) ? thread_data[tid].output->Magic(cmd, arg, 0) : 0;
   }
   void flushIcount(unsigned tid)
   {
      if (thread_data[tid].output && thread_data[tid].icount_reported > 0)
      {
         thread_data[tid].output->InstructionCount(thread_data[tid].icount_reported);
         thread_data[tid].icount_reported = 0;
      }
      m_flush[tid] = false;
   }
   void snapshot(unsigned tid)
   {
      m_prior_fs = magic(tid, SIM_CMD_GET_SIM_TIME);
      m_prior_l2 = magic(tid, SIM_CMD_GET_L2_COUNT);
      m_prior_l2miss = magic(tid, SIM_CMD_GET_L2_MISS_COUNT);
   }
   /* The simulator's instrumentation mode follows the recorder's: detailed
      for a region that is measured; for one that is not, cache warm-up
      (memory accesses still reach the caches and TLBs, nothing is timed) so
      that the next measured region does not start cold, or plain instruction
      counting when the warm-up is off. */
   void gotoDetailed(unsigned tid)
   {
      for (unsigned t = 0; t < NT; t++) m_flush[t] = true;
      flushIcount(tid);
      setInstrumentationMode(Sift::ModeDetailed);
      magic(tid, SIM_CMD_INSTRUMENT_MODE, SIM_OPT_INSTRUMENT_DETAILED);
      snapshot(tid);
      m_mode = Mode::Detailed;
   }
   void gotoFastForward(unsigned tid)
   {
      setInstrumentationMode(m_warmup ? Sift::ModeMemory : Sift::ModeIcount);
      magic(tid, SIM_CMD_INSTRUMENT_MODE, m_warmup ? SIM_OPT_INSTRUMENT_WARMUP : SIM_OPT_INSTRUMENT_FASTFORWARD);
      for (unsigned t = 0; t < NT; t++) m_flush[t] = true;
      flushIcount(tid);
      m_mode = Mode::FastForward;
   }
   /* instructions of every thread since the region began */
   uint64_t sinceCut() const
   {
      uint64_t n = 0;
      for (unsigned t = 0; t < m_nthreads; t++) n += m_ins[t] - m_region_ins[t];
      return n;
   }
   void markRegionStart(ADDRINT pc)
   {
      m_region_pc = pc;
      m_arrived.clear();
      m_region_lag = 0;
      for (unsigned t = 0; t < NT; t++) { m_region_ins[t] = m_ins[t]; for (unsigned d = 0; d < DIM; d++) m_region_bbv[t][d] = m_bbv[t]->getDimension(d); }
   }
   void start(unsigned tid, ADDRINT pc)
   {
      m_started = true;
      m_mode = Mode::Detailed;                    /* beginROI has just set the recorder to detailed */
      markRegionStart(pc);
      snapshot(tid);
      if (KnobVerbose.Value())
         std::cerr << "[PacSim] sampling starts at ROI begin in detailed mode; regions of " << m_threshold << " instructions, at least " << m_min << std::endl;
   }
   /* the region that just ended: its instructions, its BBV, its cluster */
   void endRegion(unsigned tid, ADDRINT pc, RecordType type, bool can_measure)
   {
      Region r;
      r.type = type; r.pc = m_region_pc; r.lag = m_region_lag;
      r.ins.assign(m_nthreads, 0); r.bbv.assign(m_nthreads * DIM, 0.0);
      for (unsigned t = 0; t < m_nthreads; t++)
      {
         r.ins[t] = m_ins[t] - m_region_ins[t];
         if (r.ins[t] > r.max_ins) r.max_ins = r.ins[t];
      }
      /* a thread's vector counts when it ran a meaningful share of the region;
         one that only turned around at a barrier contributes nothing */
      const uint64_t share = r.max_ins / 100 > 1000 ? r.max_ins / 100 : 1000;
      r.mix.assign(DIM, 0.0);
      for (unsigned t = 0; t < m_nthreads; t++)
      {
         if (r.ins[t] >= share)
         {
            r.nactive++;
            for (unsigned d = 0; d < DIM; d++) r.mix[d] += (double)(m_bbv[t]->getDimension(d) - m_region_bbv[t][d]);
            double norm = 0;
            for (unsigned d = 0; d < DIM; d++) { double v = (double)(m_bbv[t]->getDimension(d) - m_region_bbv[t][d]) / (double)r.ins[t]; r.bbv[t * DIM + d] = v; norm += v * v; }
            norm = std::sqrt(norm);
            if (norm > 0) for (unsigned d = 0; d < DIM; d++) r.bbv[t * DIM + d] /= norm;
         }
      }
      {
         double norm = 0;
         for (unsigned d = 0; d < DIM; d++) norm += r.mix[d] * r.mix[d];
         norm = std::sqrt(norm);
         if (norm > 0) for (unsigned d = 0; d < DIM; d++) r.mix[d] /= norm;
      }
      const bool measured = m_mode == Mode::Detailed && can_measure;
      if (measured)
      {
         const uint64_t fs = magic(tid, SIM_CMD_GET_SIM_TIME), l2 = magic(tid, SIM_CMD_GET_L2_COUNT), l2miss = magic(tid, SIM_CMD_GET_L2_MISS_COUNT);
         r.measured(fs - m_prior_fs, l2 - m_prior_l2, l2miss - m_prior_l2miss);
         m_prior_fs = fs; m_prior_l2 = l2; m_prior_l2miss = l2miss;
      }

      r.cluster = m_clusterer.assign(r.mix, r.nactive, measured);
      /* the cut point this region started at learns what it turned out to be */
      Predictor &p = m_predictors[m_region_pc];
      m_cluster_history.push_back(r.cluster);
      p.history.learn(m_cluster_history);
      std::map<uint64_t, Region>::iterator k = p.known.find(r.cluster);
      if (measured)
      {
         r.predict = k == p.known.end() ? Predict::D2D : Predict::D2F;
         if (k == p.known.end()) p.known[r.cluster] = r; else k->second.absorb(r);
         std::map<uint64_t, Region>::iterator g = m_global.find(r.cluster);
         if (g == m_global.end()) m_global[r.cluster] = r; else g->second.absorb(r);
      }
      else
         r.predict = k == p.known.end() ? Predict::F2D : Predict::F2F;   /* its time comes at the end, from the latest measurement of its cluster */
      p.last_cluster = r.cluster; p.any = true;
      m_regions.push_back(r);
      if (KnobVerbose.Value())
         std::cerr << "[PacSim] region " << m_regions.size() << " at pc 0x" << std::hex << r.pc << std::dec << " type " << (int)type
                   << ": " << r.max_ins << " instructions (max over threads), cluster " << r.cluster << (measured ? ", measured " : ", extrapolated ") << r.fs << " fs" << std::endl;
      /* the next region starts here */
      markRegionStart(pc);
   }
   /* detail, unless the region about to start at pc is predicted to repeat a measured cluster */
   Mode decide(ADDRINT pc)
   {
      std::map<ADDRINT, Predictor>::iterator it = m_predictors.find(pc);
      if (it == m_predictors.end() || !it->second.any)
         return Mode::Detailed;
      Predictor &p = it->second;
      uint64_t cluster = p.last_cluster;
      if (p.known.size() + 1 > 1) p.history.predict(m_cluster_history, cluster);
      std::map<uint64_t, Region>::iterator k = p.known.find(cluster);
      if (k != p.known.end())
         return k->second.measurements >= m_min_measurements ? Mode::FastForward : Mode::Detailed;
      std::map<uint64_t, Region>::iterator g = m_global.find(cluster);
      if (g != m_global.end() && g->second.measurements >= m_min_measurements) { p.known[cluster] = g->second; return Mode::FastForward; }
      return Mode::Detailed;
   }
   void cut(unsigned tid, ADDRINT pc, RecordType type, bool can_measure = true)
   {
      if (sinceCut() < m_min && type != RecordType::EndPgm)
         return;                                  /* too short to be a region of its own: it joins the next */
      endRegion(tid, pc, type, can_measure);
      if (type == RecordType::EndPgm)
         return;
      const Mode next = decide(pc);
      if (next != m_mode)
      {
         if (next == Mode::Detailed) gotoDetailed(tid); else gotoFastForward(tid);
      }
      const uint64_t now = magic(tid, SIM_CMD_GET_SIM_TIME), mine = magic(tid, SIM_CMD_GET_CORE_TIME);
      m_region_lag = now > mine ? now - mine : 0;
      if (KnobVerbose.Value())
         std::cerr << "[PacSim] thread " << tid << " starts region " << m_regions.size() + 1 << " " << (next == Mode::Detailed ? "measured" : "extrapolated") << ", its core " << m_region_lag << " fs behind the run" << std::endl;
   }
   void repair()
   {
      /* an unsolved region: the latest measurement of its cluster at its own
         cut point, at any cut point, else the nearest measured region by
         BBV, else the average over everything measured */
      for (size_t i = 0; i < m_regions.size(); i++)
      {
         Region &r = m_regions[i];
         if (r.solved) continue;
         std::map<ADDRINT, Predictor>::iterator p = m_predictors.find(r.pc);
         if (p != m_predictors.end())
         {
            std::map<uint64_t, Region>::iterator k = p->second.known.find(r.cluster);
            if (k != p->second.known.end() && k->second.level == Level::FromSelf) { r.extrapolate(k->second.time_per_ins, Level::FromLocal); continue; }
         }
         std::map<uint64_t, Region>::iterator g = m_global.find(r.cluster);
         if (g != m_global.end()) { r.extrapolate(g->second.time_per_ins, Level::FromGlobal); continue; }
      }
      for (size_t i = 0; i < m_regions.size(); i++)
      {
         Region &r = m_regions[i];
         if (r.solved) continue;
         const Region *best = NULL; double bd = 0;
         for (size_t j = 0; j < m_regions.size(); j++)
         {
            if (j == i || !m_regions[j].solved || m_regions[j].level != Level::FromSelf
                || m_regions[j].type == RecordType::EndPgm) continue;   /* the end region is a few thousand instructions of teardown, no measure of anything */
            const double d = l1(r.mix, m_regions[j].mix) + (m_regions[j].nactive == r.nactive ? 0.0 : 100.0);   /* the same thread count first */
            if (!best || d < bd) { best = &m_regions[j]; bd = d; }
         }
         if (best)
            r.extrapolate(best->time_per_ins, Level::FromOtherCluster);
      }
      uint64_t sum_fs = 0, sum_ins = 0;
      for (size_t i = 0; i < m_regions.size(); i++) if (m_regions[i].solved) { sum_fs += m_regions[i].fs; sum_ins += m_regions[i].max_ins; }
      const double avg = sum_ins ? (double)sum_fs / (double)sum_ins : 0;
      for (size_t i = 0; i < m_regions.size(); i++) if (!m_regions[i].solved) m_regions[i].extrapolate(avg, Level::FromGlobalAvg);
   }
   void write()
   {
      uint64_t time_sum = 0;
      for (size_t i = 0; i < m_regions.size(); i++) time_sum += m_regions[i].fs;
      const std::string path = KnobPacSimDir.Value() + "/pacsim_regions.json";
      std::ofstream out(path.c_str());
      out << "{ \"time_sum\": " << time_sum << ", \"time_cluster\": 0, \"time_prediction\": 0,\n\"feature_stack\": [\n";
      for (size_t i = 0; i < m_regions.size(); i++) out << (i ? ",\n" : "") << m_regions[i].json(m_nthreads);
      out << "\n] }\n";
      out.close();
      unsigned measured = 0;
      for (size_t i = 0; i < m_regions.size(); i++) if (m_regions[i].level == Level::FromSelf) measured++;
      std::cerr << "[PacSim] " << m_regions.size() << " regions, " << measured << " simulated in detail, " << m_predictors.size()
                << " cut points; wrote " << path << std::endl;
   }
public:
   Sampler()
      : m_threshold(KnobPacSimRegionSize.Value() * 1000), m_min(KnobPacSimMinRegionSize.Value() * 1000)
      , m_cluster_threshold(KnobPacSimClusterThreshold.Value()), m_warmup(KnobPacSimWarmup.Value()), m_min_measurements(KnobPacSimMinMeasurements.Value()), m_clusterer(m_cluster_threshold)
      , m_ins(NT, 0), m_region_ins(NT, 0), m_bbv(NT, (Bbv *)NULL), m_region_bbv(NT, std::vector<uint64_t>(DIM, 0)), m_flush(NT, false), m_active(NT, false)
   {
      for (unsigned t = 0; t < NT; t++) m_bbv[t] = new Bbv();
      PIN_MutexInit(&m_mutex);
   }
   /* every basic block, every thread, every mode */
   void block(THREADID tid, uint32_t n, ADDRINT addr)
   {
      m_ins[tid] += n;
      m_bbv[tid]->count(addr, n);
      if (m_flush[tid]) flushIcount(tid);
   }
   void roiBegin(unsigned tid)
   {
      PIN_MutexLock(&m_mutex);
      if (!m_started && !m_finished) start(tid, 0);
      PIN_MutexUnlock(&m_mutex);
   }
   /* a loop back-edge, on any thread: a cut point once the budget is spent */
   void backEdge(THREADID tid, ADDRINT addr)
   {
      if (!m_started || m_finished || !in_roi || !thread_data[tid].output) return;
      if (sinceCut() < m_threshold) return;
      if (!PIN_MutexTryLock(&m_mutex)) return;
      if (!m_finished && sinceCut() >= m_threshold)
         cut(tid, addr, RecordType::IntraBarrier);
      PIN_MutexUnlock(&m_mutex);
   }
   /* threads that ran a meaningful share of the region so far */
   unsigned activeThreads() const
   {
      uint64_t mx = 0;
      for (unsigned t = 0; t < m_nthreads; t++) if (m_ins[t] - m_region_ins[t] > mx) mx = m_ins[t] - m_region_ins[t];
      const uint64_t share = mx / 100 > 1000 ? mx / 100 : 1000;
      unsigned n = 0;
      for (unsigned t = 0; t < m_nthreads; t++) if (m_ins[t] - m_region_ins[t] >= share) n++;
      return n;
   }
   /* a synchronisation point: a barrier, or the entry of an OpenMP region.
      The region ends with the last active thread's arrival, so that the
      threads' phases line up inside it -- cutting at the first arrival would
      put the other threads' tails into the next region, and a tail run by
      one thread looks nothing like the phase it belongs to. */
   void syncPoint(THREADID tid, ADDRINT addr)
   {
      if (!m_started || m_finished || !in_roi || !thread_data[tid].output) return;
      if (!PIN_MutexTryLock(&m_mutex)) return;
      if (!m_finished)
      {
         std::set<unsigned> &here = m_arrived[addr];
         here.insert(tid);
         if (here.size() >= activeThreads() && sinceCut() >= m_min)
            cut(tid, addr, RecordType::MultiThread);
      }
      PIN_MutexUnlock(&m_mutex);
   }
   void threadStart(THREADID tid) { m_active[tid] = true; if (tid + 1 > m_nthreads) m_nthreads = tid + 1; }
   /* the end: at ROI end (the last region can still be measured), at _exit,
      or at Pin's fini (the channels are closed by then, so nothing is asked) */
   void finish(unsigned tid, ADDRINT pc, bool can_measure)
   {
      /* a cut in flight on another thread finishes within its simulator
         round trip; wait for it, but not forever (that thread's answer may
         need instructions this thread, in endROI, can no longer deliver) */
      bool locked = false;
      for (int i = 0; i < 2000 && !(locked = PIN_MutexTryLock(&m_mutex)); i++)
         usleep(1000);
      if (!m_finished && m_started)
      {
         m_finished = true;
         cut(tid, pc, RecordType::EndPgm, can_measure);
         repair();
         write();
      }
      if (locked) PIN_MutexUnlock(&m_mutex);
   }
};

static Sampler *g_sampler = NULL;

static VOID onBlock(THREADID tid, UINT32 n, ADDRINT addr) { g_sampler->block(tid, n, addr); }
static VOID onBackEdge(THREADID tid, ADDRINT addr) { g_sampler->backEdge(tid, addr); }
static VOID onSync(THREADID tid, ADDRINT addr) { g_sampler->syncPoint(tid, addr); }
static VOID onExit(THREADID tid, ADDRINT addr) { g_sampler->finish(tid, addr, in_roi && thread_data[tid].output); }

static VOID traceCallback(TRACE trace, VOID *)
{
   for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
      BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)onBlock, IARG_THREAD_ID, IARG_UINT32, BBL_NumIns(bbl), IARG_ADDRINT, BBL_Address(bbl), IARG_END);
   INS tail = BBL_InsTail(TRACE_BblTail(trace));
   if (INS_IsDirectControlFlow(tail) && INS_HasFallThrough(tail) && INS_DirectControlFlowTargetAddress(tail) < INS_Address(tail))
      INS_InsertPredicatedCall(tail, IPOINT_BEFORE, (AFUNPTR)onBackEdge, IARG_THREAD_ID, IARG_INST_PTR, IARG_END);
   RTN rtn = TRACE_Rtn(trace);
   if (RTN_Valid(rtn) && RTN_Address(rtn) == TRACE_Address(trace))
   {
      const std::string name = RTN_Name(rtn);
      if (name.find("_omp_fn") != std::string::npos)          /* a GCC OpenMP outlined region */
         INS_InsertPredicatedCall(BBL_InsHead(TRACE_BblHead(trace)), IPOINT_BEFORE, (AFUNPTR)onSync, IARG_THREAD_ID, IARG_INST_PTR, IARG_END);
   }
}

static VOID routineCallback(RTN rtn, VOID *)
{
   const std::string name = RTN_Name(rtn);
   if (name == "_exit" || name == "_Exit")
   {
      RTN_Open(rtn);
      RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)onExit, IARG_THREAD_ID, IARG_INST_PTR, IARG_END);
      RTN_Close(rtn);
   }
   else if (name == "pthread_barrier_wait" || name == "GOMP_barrier" || name == "GOMP_parallel" || name == "__kmp_barrier"
            || name == "__kmpc_barrier" || name == "__kmp_invoke_microtask" || name == "MPI_Barrier" || name == "PMPI_Barrier")
   {
      /* a synchronisation point, keyed by its call site (the barrier
         routine itself is the same address for every barrier) */
      RTN_Open(rtn);
      RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)onSync, IARG_THREAD_ID, IARG_RETURN_IP, IARG_END);
      RTN_Close(rtn);
   }
}

static VOID threadStart(THREADID tid, CONTEXT *, INT32, VOID *) { g_sampler->threadStart(tid); }
static VOID fini(INT32, VOID *) { g_sampler->finish(0, 0, false); }

void roiBegin(int tid)
{
   if (g_sampler)
      g_sampler->roiBegin(tid);
}

void roiEnd(int tid)
{
   if (g_sampler)
      g_sampler->finish(tid, 0, true);
}

void activate()
{
   g_sampler = new Sampler();
   std::cerr << "[PacSim] online sampling enabled (regions of " << KnobPacSimRegionSize.Value() << "K instructions, cluster threshold "
             << KnobPacSimClusterThreshold.Value() << ", " << (KnobPacSimWarmup.Value() ? "cache warm-up" : "instruction counting") << " between measured regions, output " << KnobPacSimDir.Value() << ")" << std::endl;
   TRACE_AddInstrumentFunction(traceCallback, NULL);
   RTN_AddInstrumentFunction(routineCallback, NULL);
   PIN_AddThreadStartFunction(threadStart, NULL);
   PIN_AddFiniFunction(fini, NULL);
}

}
