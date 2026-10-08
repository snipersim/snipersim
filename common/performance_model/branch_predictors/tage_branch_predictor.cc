#include "tage_branch_predictor.h"
#include "stats.h"

constexpr std::array<int, TAGEBranchPredictor::NHIST> TAGEBranchPredictor::HIST_LENGTHS;

TAGEBranchPredictor::TAGEBranchPredictor(String name, core_id_t core_id)
   : BranchPredictor(name, core_id)
   , m_bimodal(1 << LOGB, 2)
   , m_phist(0)
   , m_use_alt(8)
   , m_seed(0xDEADBEEF)
   , m_tagged_provided(0)
   , m_allocations(0)
{
   registerStatsMetric("tage", core_id, "tagged_provided", &m_tagged_provided);
   registerStatsMetric("tage", core_id, "allocations", &m_allocations);
   for (int t = 0; t < NHIST; ++t)
   {
      m_tables[t].resize(1 << LOGG);
      m_fold_index.emplace_back(HIST_LENGTHS[t], LOGG);
      m_fold_tag1.emplace_back(HIST_LENGTHS[t], TBITS);
      m_fold_tag2.emplace_back(HIST_LENGTHS[t], TBITS - 1);
   }
}

UInt32 TAGEBranchPredictor::index(IntPtr ip, int t) const
{
   uint64_t f = m_fold_index[t].value();
   uint64_t p = m_phist & ((1ULL << std::min(HIST_LENGTHS[t], PHIST_BITS)) - 1);
   return ((ip >> 2) ^ (ip >> (2 + LOGG - t)) ^ f ^ p) & ((1 << LOGG) - 1);
}

uint16_t TAGEBranchPredictor::tag(IntPtr ip, int t) const
{
   uint64_t v = (ip >> 2) ^ m_fold_tag1[t].value() ^ (m_fold_tag2[t].value() << 1);
   return v & ((1 << TBITS) - 1);
}

void TAGEBranchPredictor::lookup(IntPtr ip)
{
   State &s = m_state;
   s.ip = ip;
   s.valid = true;
   s.provider = s.alt = -1;
   for (int t = 0; t < NHIST; ++t)
   {
      s.index[t] = index(ip, t);
      s.tag[t] = tag(ip, t);
   }
   for (int t = NHIST - 1; t >= 0; --t)
   {
      const Entry &e = m_tables[t][s.index[t]];
      if (e.valid && e.tag == s.tag[t])
      {
         if (s.provider < 0)
            s.provider = t;
         else
         {
            s.alt = t;
            break;
         }
      }
   }
   bool bimodal = m_bimodal[(ip >> 2) & ((1 << LOGB) - 1)] >= 2;
   s.alt_pred = (s.alt >= 0) ? m_tables[s.alt][s.index[s.alt]].ctr >= 0 : bimodal;
   if (s.provider < 0)
   {
      s.pred = s.alt_pred;
      return;
   }
   const Entry &p = m_tables[s.provider][s.index[s.provider]];
   s.provider_pred = p.ctr >= 0;
   // A newly allocated (weak, not yet useful) entry defers to the alternate
   // while the alternate has been the better of the two.
   bool weak_new = (p.ctr == 0 || p.ctr == -1) && p.useful == 0;
   s.pred = (weak_new && m_use_alt >= 8) ? s.alt_pred : s.provider_pred;
}

bool TAGEBranchPredictor::predict(bool indirect, IntPtr ip, IntPtr target)
{
   lookup(ip);
   return m_state.pred;
}

void TAGEBranchPredictor::update(bool predicted, bool actual, bool indirect, IntPtr ip, IntPtr target)
{
   updateCounters(predicted, actual);

   if (!m_state.valid || m_state.ip != ip)
      lookup(ip);
   State &s = m_state;
   s.valid = false;

   if (s.provider >= 0)
   {
      ++m_tagged_provided;
      Entry &p = m_tables[s.provider][s.index[s.provider]];
      if ((p.ctr == 0 || p.ctr == -1) && p.useful == 0 && s.alt_pred != s.provider_pred)
      {
         if (s.alt_pred == actual) { if (m_use_alt < 15) ++m_use_alt; }
         else                      { if (m_use_alt > 0) --m_use_alt; }
      }
      if (actual) { if (p.ctr < CTR_MAX) ++p.ctr; }
      else        { if (p.ctr > CTR_MIN) --p.ctr; }
      if (s.provider_pred != s.alt_pred)
      {
         if (s.provider_pred == actual) { if (p.useful < U_MAX) ++p.useful; }
         else                           { if (p.useful > 0) --p.useful; }
      }
      // The alternate learns too while the provider is still new.
      if (p.useful == 0 && s.alt < 0)
      {
         uint8_t &b = m_bimodal[(ip >> 2) & ((1 << LOGB) - 1)];
         if (actual && b < 3) ++b; else if (!actual && b > 0) --b;
      }
   }
   else
   {
      uint8_t &b = m_bimodal[(ip >> 2) & ((1 << LOGB) - 1)];
      if (actual && b < 3) ++b; else if (!actual && b > 0) --b;
   }

   // On a misprediction, allocate one entry in a longer-history table whose
   // slot is not useful, picked at random among the candidates; if none is
   // free, age the candidates so that one will be.
   if (s.pred != actual)
   {
      int start = s.provider + 1;
      int free = 0;
      for (int t = start; t < NHIST; ++t)
         if (m_tables[t][s.index[t]].useful == 0)
            ++free;
      if (free)
      {
         int pick = rnd() % free;
         for (int t = start; t < NHIST; ++t)
         {
            Entry &e = m_tables[t][s.index[t]];
            if (e.useful != 0)
               continue;
            if (pick-- == 0)
            {
               e.valid = true;
               e.tag = s.tag[t];
               e.ctr = actual ? 0 : -1;
               ++m_allocations;
               break;
            }
         }
      }
      else
      {
         for (int t = start; t < NHIST; ++t)
            if (m_tables[t][s.index[t]].useful > 0)
               --m_tables[t][s.index[t]].useful;
      }
   }

   for (int t = 0; t < NHIST; ++t)
   {
      m_fold_index[t].push(actual);
      m_fold_tag1[t].push(actual);
      m_fold_tag2[t].push(actual);
   }
   m_phist = ((m_phist << 1) | ((ip >> 2) & 1)) & ((1ULL << PHIST_BITS) - 1);
}

uint32_t TAGEBranchPredictor::rnd()
{
   m_seed ^= m_seed << 13;
   m_seed ^= m_seed >> 17;
   m_seed ^= m_seed << 5;
   return m_seed;
}
