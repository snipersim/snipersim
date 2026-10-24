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
   fs = r.fs; max_ins = r.max_ins; time_per_ins = r.time_per_ins; measurements++;
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

std::string Region::json(unsigned nthreads) const
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
   , m_clusterer(options.cluster_threshold)
   , m_ins(m_nt, 0), m_region_ins(m_nt, 0), m_bbv(m_nt, (Bbv *)NULL), m_region_bbv(m_nt, std::vector<uint64_t>(DIM, 0)), m_flush(m_nt, false)
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
   const bool warmup = m_options.fast_forward == FastForwardKind::Warmup;
   m_fe.setFastForward(m_options.fast_forward);
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
   std::map<uint64_t, Predictor>::iterator it = m_predictors.find(pc);
   if (it == m_predictors.end() || !it->second.any)
      return Mode::Detailed;
   Predictor &p = it->second;
   uint64_t cluster = p.last_cluster;
   p.history.predict(m_cluster_history, cluster);
   std::map<uint64_t, Region>::iterator k = p.known.find(cluster);
   if (k != p.known.end())
      return k->second.measurements >= m_options.min_measurements ? Mode::FastForward : Mode::Detailed;
   std::map<uint64_t, Region>::iterator g = m_global.find(cluster);
   if (g != m_global.end() && g->second.measurements >= m_options.min_measurements) { p.known[cluster] = g->second; return Mode::FastForward; }
   return Mode::Detailed;
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
         const double d = l1(r.mix, m_regions[j].mix) + (m_regions[j].nactive == r.nactive ? 0.0 : 100.0);
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

void Sampler::write()
{
   uint64_t time_sum = 0;
   for (size_t i = 0; i < m_regions.size(); i++) time_sum += m_regions[i].fs;
   const std::string path = m_options.output_dir + "/pacsim_regions.json";
   std::ofstream out(path.c_str());
   out << "{ \"time_sum\": " << time_sum << ", \"time_cluster\": 0, \"time_prediction\": 0,\n\"feature_stack\": [\n";
   for (size_t i = 0; i < m_regions.size(); i++) out << (i ? ",\n" : "") << m_regions[i].json(m_nthreads);
   out << "\n] }\n";
   out.close();
   unsigned measured = 0;
   for (size_t i = 0; i < m_regions.size(); i++) if (m_regions[i].level == Level::FromSelf) measured++;
   std::stringstream s;
   s << "[PacSim] " << m_regions.size() << " regions, " << measured << " simulated in detail, " << m_predictors.size() << " cut points; wrote " << path;
   m_fe.log(s.str());
}

void Sampler::block(unsigned tid, uint32_t n, uint64_t addr)
{
   m_ins[tid] += n;
   m_bbv[tid]->count(addr, n);
   if (m_flush[tid]) flushIcount(tid);
}

void Sampler::roiBegin(unsigned tid)
{
   m_fe.lock();
   if (!m_started && !m_finished) start(tid, 0);
   m_fe.unlock();
}

void Sampler::backEdge(unsigned tid, uint64_t addr)
{
   if (!m_started || m_finished || !m_fe.inRoi() || !m_fe.channelAlive(tid)) return;
   if (sinceCut() < m_threshold) return;
   if (!m_fe.tryLock()) return;
   if (!m_finished && sinceCut() >= m_threshold)
      cut(tid, addr, RecordType::IntraBarrier);
   m_fe.unlock();
}

/* The region ends with the last active thread's arrival, so that the
   threads' phases line up inside it -- cutting at the first arrival would
   put the other threads' tails into the next region, and a tail run by one
   thread looks nothing like the phase it belongs to. */
void Sampler::syncPoint(unsigned tid, uint64_t addr)
{
   if (!m_started || m_finished || !m_fe.inRoi() || !m_fe.channelAlive(tid)) return;
   if (!m_fe.tryLock()) return;
   if (!m_finished)
   {
      std::set<unsigned> &here = m_arrived[addr];
      here.insert(tid);
      if (here.size() >= activeThreads() && sinceCut() >= m_min)
         cut(tid, addr, RecordType::MultiThread);
   }
   m_fe.unlock();
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
      write();
   }
   if (locked) m_fe.unlock();
}

}
