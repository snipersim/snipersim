#include "pacsim_sampler.h"
#include "../include/sim_api.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace pacsim
{

static const unsigned DIM = Bbv::NUM_BBV;

static double l1(const std::vector<double> &a, const std::vector<double> &b)
{
   double d = 0;
   const size_t n = a.size() < b.size() ? a.size() : b.size();
   for (size_t i = 0; i < n; i++) d += std::fabs(a[i] - b[i]);
   return d;
}

static std::string hex(uint64_t v)
{
   std::stringstream s; s << "0x" << std::hex << v; return s.str();
}

/* ------------------------------------------------------------ one region */
void Region::measured(uint64_t fs_, uint64_t l2_, uint64_t l2miss_)
{
   fs = fs_; l2 = l2_; l2miss = l2miss_;
   level = Level::FromSelf; solved = true; measurements = 1;
   time_per_ins = max_ins ? (double)fs / (double)max_ins : 0;
}

void Region::absorb(const Region &r)
{
   prev_time_per_ins = time_per_ins;
   fs = r.fs; max_ins = r.max_ins; time_per_ins = r.time_per_ins; measurements++;
   ins = r.ins; bbv = r.bbv; nactive = r.nactive;   /* the threads of the measurement that stands for the cluster */
}

/* measured often enough, and the last two measurements agree (Options::converge) */
bool Region::settled(unsigned min_measurements, double converge) const
{
   if (measurements < min_measurements) return false;
   if (converge <= 0 || measurements < 2 || time_per_ins <= 0) return true;
   const double d = time_per_ins > prev_time_per_ins ? time_per_ins - prev_time_per_ins : prev_time_per_ins - time_per_ins;
   return d <= converge * time_per_ins;
}

/* An unmeasured region: its threads' time at the rate of its cluster, less
   the lag its thread had at the start -- a core that is behind moves the
   run's time only once it has caught up. */
void Region::extrapolate(double tpi, Level from)
{
   time_per_ins = tpi;
   const double t = tpi * (double)max_ins;
   fs = t > (double)lag ? (uint64_t)(t - (double)lag) : 0;
   level = from; solved = true;
}

void Region::extrapolate(const Region &rep, Level from)
{
   extrapolate(rep.time_per_ins, from);
   has_rep = true; rep_fs = rep.fs; rep_ins = rep.ins; rep_bbv = rep.bbv;
}

/* Thread t's time in this region: its instructions at the rate of the
   representative's thread whose block mix is nearest its own, among those
   that ran a fair share of the representative (a thread that ran little of
   it was waiting, and its rate says nothing).  Without a representative,
   the region's own rate per instruction. */
double Region::threadTime(unsigned t, unsigned nthreads) const
{
   const uint64_t n = t < ins.size() ? ins[t] : 0;
   if (!n) return 0;
   if (has_rep)
   {
      uint64_t rep_max = 0;
      for (size_t u = 0; u < rep_ins.size(); u++) if (rep_ins[u] > rep_max) rep_max = rep_ins[u];
      int best = -1; double bd = 0;
      for (unsigned u = 0; u < nthreads && u < rep_ins.size(); u++)
      {
         if (!rep_ins[u] || 10 * rep_ins[u] < 3 * rep_max) continue;
         double d = 0;
         for (unsigned k = 0; k < DIM; k++)
         {
            const double a = (t * DIM + k) < bbv.size() ? bbv[t * DIM + k] : 0, b = (u * DIM + k) < rep_bbv.size() ? rep_bbv[u * DIM + k] : 0;
            d += std::fabs(a - b);
         }
         if (best < 0 || d < bd) { best = (int)u; bd = d; }
      }
      if (best >= 0)
         return (double)n * (double)rep_fs / (double)rep_ins[best];
   }
   return (double)n * time_per_ins;
}

std::string Region::json(unsigned nthreads) const
{
   std::stringstream out;
   out << "  { \"max_ins_num\": " << max_ins << ", \"fs\": " << fs << ", \"region_fs\": " << region_fs << ", \"l2count\": " << l2 << ", \"l2misscount\": " << l2miss
       << ", \"clusterid\": " << cluster << ", \"pc\": " << pc << ", \"freq\": 0"
       << ", \"predict_result_type\": " << (int)predict << ", \"recordtype\": " << (int)type
       << ", \"feature_level\": " << (int)level << ",\n    \"insnums\": [";
   for (unsigned t = 0; t < nthreads; t++) out << (t ? ", " : "") << (t < ins.size() ? ins[t] : 0);
   out << "],\n    \"bbv\": [";
   for (unsigned i = 0; i < nthreads * DIM; i++) out << (i ? ", " : "") << (i < bbv.size() ? bbv[i] : 0.0);
   out << "] }";
   return out.str();
}

/* ------------------------------------------------- clustering by block mix
 * A region joins the first cluster whose representative mix is within the
 * threshold and which had the same number of active threads (the same code
 * run by one thread -- a thread catching up with the others, which wait for
 * it at the simulator's barrier -- costs a fraction of the global time that
 * the same code run by all of them does), measured clusters searched before
 * fast-forwarded ones; otherwise it founds one. */
uint64_t Clusterer::assign(const std::vector<double> &bbv, unsigned nactive, bool measured)
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

/* --------------------------------------- what comes next, from the history
 * The next cluster of a cut point is predicted from the sequence of clusters
 * seen so far: the longest suffix of the history that has occurred before
 * votes with what followed it (the trie of the original). */
void History::learn(const std::vector<uint64_t> &hist)
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

bool History::predict(const std::vector<uint64_t> &hist, uint64_t &out) const
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

/* -------------------------------------------------------------- the sampler */
Sampler::Sampler(const Options &options, FrontEnd &front_end)
   : m_options(options), m_fe(front_end), m_nt(options.max_threads)
   , m_threshold(options.region_size), m_min(options.min_region_size)
   , m_loop_min(options.loop_min == 0 ? (options.region_size / 100 > 0 ? options.region_size / 100 : 1) : options.loop_min)
   , m_clusterer(options.cluster_threshold)
   , m_ins(m_nt, 0), m_region_ins(m_nt, 0), m_bbv(m_nt, (Bbv *)NULL), m_region_bbv(m_nt, std::vector<uint64_t>(DIM, 0)), m_flush(m_nt, false)
   , m_last_edge(m_nt)
{
   for (unsigned t = 0; t < m_nt; t++) m_bbv[t] = new Bbv();
}

Sampler::~Sampler()
{
   for (unsigned t = 0; t < m_nt; t++) delete m_bbv[t];
}

/* the simulator's counters, asked through the calling thread's own channel;
   only while its channel is alive and inside the ROI, since after ROI end
   the channel has been ended and a question would wait forever */
uint64_t Sampler::magic(unsigned tid, uint64_t cmd, uint64_t arg)
{
   return (m_fe.inRoi() && m_fe.channelAlive(tid)) ? m_fe.magic(tid, cmd, arg) : 0;
}

void Sampler::flushIcount(unsigned tid)
{
   m_fe.flushCounts(tid);
   m_flush[tid] = false;
}

void Sampler::snapshot(unsigned tid)
{
   m_prior_fs = magic(tid, SIM_CMD_GET_SIM_TIME);
   m_prior_l2 = magic(tid, SIM_CMD_GET_L2_COUNT);
   m_prior_l2miss = magic(tid, SIM_CMD_GET_L2_MISS_COUNT);
}

/* The simulator's instrumentation mode follows the front end's: detailed for
   a region that is measured; for one that is not, cache warm-up (memory
   accesses still reach the caches and TLBs, nothing is timed) so that the
   next measured region does not start cold, or plain instruction counting. */
void Sampler::gotoDetailed(unsigned tid)
{
   for (unsigned t = 0; t < m_nt; t++) m_flush[t] = true;
   flushIcount(tid);
   m_fe.setDetailed();
   magic(tid, SIM_CMD_INSTRUMENT_MODE, SIM_OPT_INSTRUMENT_DETAILED);
   snapshot(tid);
   m_mode = Mode::Detailed;
}

void Sampler::gotoFastForward(unsigned tid)
{
   /* With a warm-up window the region starts as counting; block() turns the
      warm-up on when the region is within the window of its budget. */
   const bool warmup = m_options.fast_forward == FastForwardKind::Warmup && m_options.warmup_window == 0;
   m_warming = false;
   m_fe.setFastForward(warmup ? FastForwardKind::Warmup : FastForwardKind::Icount);
   magic(tid, SIM_CMD_INSTRUMENT_MODE, warmup ? SIM_OPT_INSTRUMENT_WARMUP : SIM_OPT_INSTRUMENT_FASTFORWARD);
   for (unsigned t = 0; t < m_nt; t++) m_flush[t] = true;
   flushIcount(tid);
   m_mode = Mode::FastForward;
}

/* instructions of every thread since the region began */
uint64_t Sampler::sinceCut() const
{
   uint64_t n = 0;
   for (unsigned t = 0; t < m_nthreads; t++) n += m_ins[t] - m_region_ins[t];
   return n;
}

/* threads that ran a meaningful share of the region so far */
unsigned Sampler::activeThreads() const
{
   uint64_t mx = 0;
   for (unsigned t = 0; t < m_nthreads; t++) if (m_ins[t] - m_region_ins[t] > mx) mx = m_ins[t] - m_region_ins[t];
   const uint64_t share = mx / 100 > 1000 ? mx / 100 : 1000;
   unsigned n = 0;
   for (unsigned t = 0; t < m_nthreads; t++) if (m_ins[t] - m_region_ins[t] >= share) n++;
   return n;
}

void Sampler::markRegionStart(uint64_t pc)
{
   m_region_pc = pc;
   m_arrived.clear();
   m_region_lag = 0;
   for (unsigned t = 0; t < m_nt; t++) { m_region_ins[t] = m_ins[t]; for (unsigned d = 0; d < DIM; d++) m_region_bbv[t][d] = m_bbv[t]->getDimension(d); }
}

void Sampler::start(unsigned tid, uint64_t pc)
{
   m_started = true;
   m_mode = Mode::Detailed;                    /* the ROI has just opened in detailed mode */
   markRegionStart(pc);
   snapshot(tid);
   if (m_options.verbose)
   {
      std::stringstream s;
      s << "[PacSim] sampling starts at ROI begin in detailed mode; regions of " << m_threshold << " instructions, at least " << m_min;
      m_fe.log(s.str());
   }
}

/* the region that just ended: its instructions, its vectors, its cluster */
void Sampler::endRegion(unsigned tid, uint64_t pc, RecordType type, bool can_measure)
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
   m_global_history.learn(m_cluster_history);
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
   if (m_options.verbose)
   {
      std::stringstream s;
      s << "[PacSim] region " << m_regions.size() << " at pc " << hex(r.pc) << " type " << (int)type
        << ": " << r.max_ins << " instructions (max over threads), cluster " << r.cluster << (measured ? ", measured " : ", extrapolated ") << r.fs << " fs";
      m_fe.log(s.str());
   }
   /* the next region starts here */
   markRegionStart(pc);
}

/* detail, unless the region about to start at pc is predicted to repeat a cluster measured often enough */
Mode Sampler::decide(uint64_t pc)
{
   /* The cluster expected next: this cut point's history over the run's
      cluster sequence, else what followed this cut point last time, else
      what the run saw last -- a cut point seen for the first time is not a
      reason to measure what the run already knows.  A loop with several
      back-edges (a compiler's token loop: 23 cut points in the agent's
      generate()) otherwise learned every cluster afresh at each of them,
      two measurements a time, and measured 49 regions of 83 where the
      clusters repeated after a dozen. */
   std::map<uint64_t, Predictor>::iterator it = m_predictors.find(pc);
   Predictor *p = (it != m_predictors.end() && it->second.any) ? &it->second : NULL;
   uint64_t cluster = p ? p->last_cluster : (m_cluster_history.empty() ? 0 : m_cluster_history.back());
   if (p)
      p->history.predict(m_cluster_history, cluster);
   else if (!m_cluster_history.empty())
      m_global_history.predict(m_cluster_history, cluster);
   /* How often the cluster was measured: here or anywhere; a representative
      measured at another cut point is as good as one measured at this one */
   const Region *rep = NULL;
   if (p)
   {
      std::map<uint64_t, Region>::iterator k = p->known.find(cluster);
      if (k != p->known.end()) rep = &k->second;
   }
   std::map<uint64_t, Region>::iterator g = m_global.find(cluster);
   if (g != m_global.end() && (!rep || g->second.measurements > rep->measurements))
   {
      rep = &g->second;
      if (p && rep->settled(m_options.min_measurements, m_options.converge)) p->known[cluster] = g->second;
   }
   return rep && rep->settled(m_options.min_measurements, m_options.converge) ? Mode::FastForward : Mode::Detailed;
}

void Sampler::cut(unsigned tid, uint64_t pc, RecordType type, bool can_measure)
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
   if (m_options.verbose)
   {
      std::stringstream s;
      s << "[PacSim] thread " << tid << " starts region " << m_regions.size() + 1 << " " << (next == Mode::Detailed ? "measured" : "extrapolated") << ", its core " << m_region_lag << " fs behind the run";
      m_fe.log(s.str());
   }
}

/* an unsolved region: the latest measurement of its cluster at its own cut
   point, at any cut point, else the nearest measured region by block mix
   (the same thread count first), else the average over everything measured */
void Sampler::repair()
{
   for (size_t i = 0; i < m_regions.size(); i++)
   {
      Region &r = m_regions[i];
      if (r.solved) continue;
      std::map<uint64_t, Predictor>::iterator p = m_predictors.find(r.pc);
      if (p != m_predictors.end())
      {
         std::map<uint64_t, Region>::iterator k = p->second.known.find(r.cluster);
         if (k != p->second.known.end() && k->second.level == Level::FromSelf) { r.extrapolate(k->second, Level::FromLocal); continue; }
      }
      std::map<uint64_t, Region>::iterator g = m_global.find(r.cluster);
      if (g != m_global.end()) { r.extrapolate(g->second, Level::FromGlobal); continue; }
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
         const double d = l1(r.mix, m_regions[j].mix) + (m_regions[j].nactive == r.nactive ? 0.0 : 100.0);
         if (!best || d < bd) { best = &m_regions[j]; bd = d; }
      }
      if (best)
         r.extrapolate(*best, Level::FromOtherCluster);
   }
   uint64_t sum_fs = 0, sum_ins = 0;
   for (size_t i = 0; i < m_regions.size(); i++) if (m_regions[i].solved) { sum_fs += m_regions[i].fs; sum_ins += m_regions[i].max_ins; }
   const double avg = sum_ins ? (double)sum_fs / (double)sum_ins : 0;
   for (size_t i = 0; i < m_regions.size(); i++) if (!m_regions[i].solved) m_regions[i].extrapolate(avg, Level::FromGlobalAvg);
}

/* The run's time, thread by thread, where the threads do not synchronise.

   repair() gives each region the time of its busiest thread at its
   cluster's rate, and the run is the sum: right when the threads run side
   by side in every region as they do in the full run -- a barrier between
   regions makes them.  Between loop cuts nothing does, and the sampled run
   does not keep them side by side: in the regions it fast-forwards, the
   threads advance at the fast-forward model's rates (one instruction per
   cycle plus memory latency), not the detailed model's, and they drift.
   The loop shape's threads, synchronised only on entry and exit, ended
   one sampled run with two of them running on alone for the last four
   regions, 64 ms of a 150 ms run, measured in detail and summed; and a
   fast-forwarded region held 9.8M instructions of one thread and none of
   another's, charged as if that thread had run alone (that one was the
   fast-forward barrier's doing, FastforwardPerformanceModel::handleMemoryLatency).
   Summed so, the estimate was the drifted schedule's time: 18.6%, 16.0%
   and 7.1% over the full run under QEMU, whose threads drifted further
   (-7.6% once), -1.6% and +1.1% under SDE (2026-10-29).  With the barrier
   fixed and nothing else, the threads keep in step in simulated time but
   at the fast-forward model's relative rates, and the sum was 15% to 37%
   over (three runs).

   So within a stretch of loop-cut (IntraBarrier) regions each thread's own
   time is accumulated: a measured region's time for a thread that ran
   through it, its instructions at its representative's rate (the
   representative's thread most like it, Region::threadTime) otherwise; the
   run's time is the slowest thread's.  A region cut at a synchronisation
   point, and the end, keep repair()'s time and bring every thread level
   with the slowest.  Each region's fs is then its increase of the slowest
   thread's time, so that the file's sum is still the estimate. */
void Sampler::account()
{
   const unsigned nt = m_nthreads;
   std::vector<size_t> last(nt, 0);                  /* the last region in which a thread ran a share of it (its exit path in the end region does not count) */
   for (size_t i = 0; i < m_regions.size(); i++)
   {
      const Region &r = m_regions[i];
      if (r.type == RecordType::EndPgm) continue;
      const uint64_t share = r.max_ins / 100 > 1000 ? r.max_ins / 100 : 1000;
      for (unsigned t = 0; t < nt && t < r.ins.size(); t++)
         if (r.ins[t] >= share) last[t] = i;
   }
   std::vector<double> clock(nt, 0.0);
   for (size_t i = 0; i < m_regions.size(); i++)
   {
      Region &r = m_regions[i];
      r.region_fs = r.fs;
      double before = 0;
      for (unsigned t = 0; t < nt; t++) if (clock[t] > before) before = clock[t];
      if (r.type != RecordType::IntraBarrier)
      {
         for (unsigned t = 0; t < nt; t++) clock[t] = before + (double)r.fs;
         continue;
      }
      const uint64_t share = r.max_ins / 100 > 1000 ? r.max_ins / 100 : 1000;
      for (unsigned t = 0; t < nt && t < r.ins.size(); t++)
      {
         if (!r.ins[t]) continue;
         /* measured, and running from its start to its end: the region's time */
         if (r.level == Level::FromSelf && r.ins[t] >= share && i != last[t])
            clock[t] += (double)r.fs;
         else
         {
            /* a thread that ends in a measured region ran part of it: its share
               at the region's own rates, at most all of it */
            Region self;
            const Region *rate = &r;
            if (r.level == Level::FromSelf)
            {
               self = r; self.has_rep = true; self.rep_fs = r.fs; self.rep_ins = r.ins; self.rep_bbv = r.bbv;
               rate = &self;
            }
            double tt = rate->threadTime(t, nt);
            if (r.level == Level::FromSelf && tt > (double)r.fs) tt = (double)r.fs;
            clock[t] += tt;
         }
      }
      double after = before;
      for (unsigned t = 0; t < nt; t++) if (clock[t] > after) after = clock[t];
      r.fs = (uint64_t)(after - before);
   }
}

void Sampler::write()
{
   uint64_t time_sum = 0;
   for (size_t i = 0; i < m_regions.size(); i++) time_sum += m_regions[i].fs;
   const std::string path = m_options.output_dir + "/pacsim_regions.json";
   std::ofstream out(path.c_str());
   /* "solved": every region's fs is final -- measured, or extrapolated by
      repair() from the latest measurement of its cluster; the reader must
      not redo the extrapolation from the regions measured before it (its
      legacy, for regions the recorder left at fs 0), which would let a
      run's cold first regions stand for their cluster */
   out << "{ \"time_sum\": " << time_sum << ", \"solved\": true, \"time_cluster\": 0, \"time_prediction\": 0,\n\"feature_stack\": [\n";
   for (size_t i = 0; i < m_regions.size(); i++) out << (i ? ",\n" : "") << m_regions[i].json(m_nthreads);
   out << "\n] }\n";
   out.close();
   unsigned measured = 0;
   for (size_t i = 0; i < m_regions.size(); i++) if (m_regions[i].level == Level::FromSelf) measured++;
   std::stringstream s;
   s << "[PacSim] " << m_regions.size() << " regions, " << measured << " simulated in detail, " << m_predictors.size() << " cut points; wrote " << path;
   m_fe.log(s.str());
}

/* The warm-up window: the fast-forwarded region has run to within the
   window of its budget (with `extra` more instructions counted), so the
   caches are warmed from here to the cut. */
bool Sampler::warmupDue(uint64_t extra) const
{
   return m_started && !m_finished && m_mode == Mode::FastForward && !m_warming
       && m_options.fast_forward == FastForwardKind::Warmup && m_options.warmup_window > 0
       && sinceCut() + extra + m_options.warmup_window >= m_threshold;
}

bool Sampler::block(unsigned tid, uint32_t n, uint64_t addr, bool can_block)
{
   /* An event that would ask the simulator is left undone before anything
      is counted: the second call, where the front end can wait, counts the
      block, and must not count it twice.  The flush asks for the mode. */
   if (!can_block && (m_flush[tid] || warmupDue(tid < m_nthreads ? n : 0)))
      return true;
   m_ins[tid] += n;
   m_bbv[tid]->count(addr, n);
   if (m_flush[tid]) flushIcount(tid);
   if (warmupDue(0))
   {
      if (!can_block) return false;   /* another thread's count tipped it: the next block's call starts it */
      if (m_fe.tryLock())
      {
         if (m_mode == Mode::FastForward && !m_warming)
         {
            m_warming = true;
            m_fe.setFastForward(FastForwardKind::Warmup);
            magic(tid, SIM_CMD_INSTRUMENT_MODE, SIM_OPT_INSTRUMENT_WARMUP);
            for (unsigned t = 0; t < m_nt; t++) m_flush[t] = true;
            flushIcount(tid);
         }
         m_fe.unlock();
      }
   }
   return false;
}

void Sampler::roiBegin(unsigned tid)
{
   m_fe.lock();
   if (!m_started && !m_finished) start(tid, 0);
   m_fe.unlock();
}

bool Sampler::backEdge(unsigned tid, uint64_t addr, bool conditional, bool can_block)
{
   if (!m_started || m_finished || !m_fe.inRoi() || !m_fe.channelAlive(tid)) return false;
   if (!conditional && m_options.conditional_only) return false;
   /* The loop's iteration, from this back-edge's previous firing on this
      thread (Options::loop_min).  A region boundary should be an iteration
      boundary of a loop with iterations of some length -- the loop over a
      phase, not the tight loop inside it: a cut at a tight loop's back-edge
      falls wherever the budget runs out, at a point that depends on how far
      each thread has got, and regions made that way cluster poorly (the
      barrier shape's compute phase split that way took its estimate from
      0.1% off to 5%, the loop shape's to 17%).  The front ends report every
      backward direct branch, conditional or not; the choice is made here,
      the same for both.  A back-edge qualifies when its loop's previous
      iteration ran at least loop_min instructions, and any back-edge is
      taken once twice the budget has passed without one, so a program
      whose loops are all tight still gets its regions.  With loop_min = 1
      every back-edge qualifies at once: the paper's rule. */
   bool coarse = true;
   uint64_t *last = nullptr;
   const uint64_t now = m_ins[tid];
   if (m_loop_min > 1)
   {
      last = &m_last_edge[tid][addr];
      coarse = *last != 0 && now - *last >= m_loop_min;
   }
   if (sinceCut() < m_threshold || (!coarse && sinceCut() < 2 * m_threshold))
   {
      if (last) *last = now;
      return false;
   }
   /* A cut: questions for the simulator.  Left undone with the iteration
      unrecorded, so that the second call, where the front end can wait,
      judges the same iteration -- recorded here, the second call saw an
      iteration of no instructions and never cut at a coarse loop. */
   if (!can_block) return true;
   if (last) *last = now;
   if (!m_fe.tryLock()) return false;
   if (!m_finished && sinceCut() >= m_threshold)
      cut(tid, addr, RecordType::IntraBarrier);
   m_fe.unlock();
   return false;
}

/* The region ends with the last active thread's arrival, so that the
   threads' phases line up inside it -- cutting at the first arrival would
   put the other threads' tails into the next region, and a tail run by one
   thread looks nothing like the phase it belongs to. */
bool Sampler::syncPoint(unsigned tid, uint64_t addr, bool can_block)
{
   if (!m_started || m_finished || !m_fe.inRoi() || !m_fe.channelAlive(tid)) return false;
   if (!m_fe.tryLock()) return false;
   bool deferred = false;
   if (!m_finished)
   {
      std::set<unsigned> &here = m_arrived[addr];
      here.insert(tid);            /* the arrival counts either way; a second call finds it there */
      if (here.size() >= activeThreads() && sinceCut() >= m_min)
      {
         if (can_block)
            cut(tid, addr, RecordType::MultiThread);
         else
            deferred = true;
      }
   }
   m_fe.unlock();
   return deferred;
}

void Sampler::threadStart(unsigned tid)
{
   if (tid + 1 > m_nthreads && tid < m_nt) m_nthreads = tid + 1;
}

void Sampler::finish(unsigned tid, uint64_t pc, bool can_measure)
{
   /* a cut in flight on another thread finishes within its simulator round
      trip; wait for it, but not forever (that thread's answer may need
      instructions this thread, at ROI end, can no longer deliver) */
   bool locked = false;
   for (int i = 0; i < 2000 && !(locked = m_fe.tryLock()); i++)
      usleep(1000);
   if (!m_finished && m_started)
   {
      m_finished = true;
      cut(tid, pc, RecordType::EndPgm, can_measure);
      repair();
      account();
      write();
   }
   if (locked) m_fe.unlock();
}

}
