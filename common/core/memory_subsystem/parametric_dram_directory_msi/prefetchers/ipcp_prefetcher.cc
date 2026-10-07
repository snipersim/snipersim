#include "ipcp_prefetcher.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"

#include <cstring>

IPCPPrefetcher::IPCPPrefetcher(String configName, core_id_t core_id)
   : m_core_id(core_id)
   , m_prefetch_degree(3)
   , m_spec_nl_threshold(15)
   , m_num_misses(0)
   , m_prev_sample(0)
   , m_spec_nl(false)
{
   // Both optional; the defaults are the ChampSim ones upstream compiled in
   String base = "perf_model/" + configName + "/prefetcher/ipcp/";
   if (Sim()->getCfg()->hasKey(base + "prefetch_degree"))
      m_prefetch_degree = Sim()->getCfg()->getIntArray(base + "prefetch_degree", core_id);
   if (Sim()->getCfg()->hasKey(base + "spec_nl_threshold"))
      m_spec_nl_threshold = Sim()->getCfg()->getIntArray(base + "spec_nl_threshold", core_id);

   memset(m_ghb, 0, sizeof(m_ghb));

   memset(&stats, 0, sizeof(stats));
   registerStatsMetric("ipcp", m_core_id, "pref_called", &stats.pref_called);
   registerStatsMetric("ipcp", m_core_id, "stream_prefetches", &stats.stream_prefetches);
   registerStatsMetric("ipcp", m_core_id, "cs_prefetches", &stats.cs_prefetches);
   registerStatsMetric("ipcp", m_core_id, "cplx_prefetches", &stats.cplx_prefetches);
   registerStatsMetric("ipcp", m_core_id, "nl_prefetches", &stats.nl_prefetches);
   registerStatsMetric("ipcp", m_core_id, "prefetches_issued", &stats.prefetches_issued);
}

IPCPPrefetcher::~IPCPPrefetcher()
{
}

std::vector<IntPtr>
IPCPPrefetcher::getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip)
{
   std::vector<IntPtr> pref_addr;
   if (mem_op_type == Core::WRITE)
      return pref_addr;

   stats.pref_called++;

   UInt64 curr_page = current_address >> LOG2_PAGE_SIZE;
   UInt64 cl_addr = current_address >> LOG2_BLOCK_SIZE;
   UInt64 cl_offset = cl_addr & 0x3F;
   UInt32 num_prefs = 0;

   if (!cache_hit)
      m_num_misses++;

   // Every MISSES_PER_SAMPLE misses, re-decide the speculative next-line
   // from the miss rate over the accesses since the last sample (the
   // access count stands in for cycles, as upstream)
   if (m_num_misses >= MISSES_PER_SAMPLE)
   {
      UInt64 now = stats.pref_called;
      float mpka = (now > m_prev_sample) ? ((float)m_num_misses / (float)(now - m_prev_sample)) * 1000 : 0;
      m_prev_sample = now;
      m_spec_nl = !(mpka > (float)m_spec_nl_threshold);
      m_num_misses = 0;
   }

   UInt16 ip_tag = (eip >> NUM_IP_INDEX_BITS) & ((1 << NUM_IP_TAG_BITS) - 1);
   UInt32 index = eip & ((1 << NUM_IP_INDEX_BITS) - 1);
   IPTableEntry &entry = m_ip_table[index];

   if (entry.ip_tag != ip_tag)
   {
      if (!entry.ip_valid)
      {
         // A new IP takes the entry
         entry.ip_tag = ip_tag;
         entry.last_page = curr_page;
         entry.last_cl_offset = cl_offset;
         entry.last_stride = 0;
         entry.signature = 0;
         entry.conf = 0;
         entry.str_valid = false;
         entry.str_strength = false;
         entry.str_dir = false;
         entry.ip_valid = true;
      }
      else
         entry.ip_valid = false;   // conflict: the next one to come takes it

      // And gets a next-line prefetch meanwhile
      IntPtr pf_address = (cl_addr + 1) << LOG2_BLOCK_SIZE;
      if ((pf_address >> LOG2_PAGE_SIZE) == curr_page)
      {
         pref_addr.push_back(pf_address);
         stats.nl_prefetches++;
         stats.prefetches_issued++;
      }
      return pref_addr;
   }
   entry.ip_valid = true;

   SInt64 stride = (SInt64)cl_offset - (SInt64)entry.last_cl_offset;
   if (stride == 0)
      return pref_addr;

   // Across a page boundary the offset wraps; undo that for the stride
   if (curr_page != entry.last_page)
      stride += (stride < 0) ? 64 : -64;

   // Constant stride: confidence in the last stride, replaced when it falls to zero
   entry.conf = updateConf((SInt32)stride, (SInt32)entry.last_stride, entry.conf);
   if (entry.conf == 0)
      entry.last_stride = stride;

   // Complex stride: the delta the signature predicted, likewise
   UInt16 last_signature = entry.signature;
   m_dpt[last_signature].conf = updateConf((SInt32)stride, m_dpt[last_signature].delta, m_dpt[last_signature].conf);
   if (m_dpt[last_signature].conf == 0)
      m_dpt[last_signature].delta = (SInt32)stride;

   UInt16 signature = updateSignature(last_signature, (SInt32)stride);
   entry.signature = signature;

   checkForStream(index, cl_addr);

   if (entry.str_valid)
   {
      // Stream: twice the degree, in its direction
      for (UInt32 i = 0; i < m_prefetch_degree * 2; i++)
      {
         IntPtr pf_address = (entry.str_dir ? (cl_addr + i + 1) : (cl_addr - i - 1)) << LOG2_BLOCK_SIZE;
         if ((pf_address >> LOG2_PAGE_SIZE) != curr_page)
            break;
         pref_addr.push_back(pf_address);
         stats.stream_prefetches++;
         num_prefs++;
      }
   }
   else if (entry.conf > 1 && entry.last_stride != 0)
   {
      // Constant stride
      for (UInt32 i = 0; i < m_prefetch_degree; i++)
      {
         IntPtr pf_address = (IntPtr)((SInt64)cl_addr + entry.last_stride * (SInt64)(i + 1)) << LOG2_BLOCK_SIZE;
         if ((pf_address >> LOG2_PAGE_SIZE) != curr_page)
            break;
         pref_addr.push_back(pf_address);
         stats.cs_prefetches++;
         num_prefs++;
      }
   }
   else if (m_dpt[signature].conf >= 0 && m_dpt[signature].delta != 0)
   {
      // Complex stride: follow the signature chain
      SInt32 pref_offset = 0;
      UInt16 curr_sig = signature;
      for (UInt32 i = 0; i < m_prefetch_degree; i++)
      {
         pref_offset += m_dpt[curr_sig].delta;
         IntPtr pf_address = (IntPtr)((SInt64)cl_addr + pref_offset) << LOG2_BLOCK_SIZE;
         if ((pf_address >> LOG2_PAGE_SIZE) != curr_page || m_dpt[curr_sig].conf == -1 || m_dpt[curr_sig].delta == 0)
            break;
         if (m_dpt[curr_sig].conf > 0)
         {
            pref_addr.push_back(pf_address);
            stats.cplx_prefetches++;
            num_prefs++;
         }
         curr_sig = updateSignature(curr_sig, m_dpt[curr_sig].delta);
      }
   }

   // Nothing classified: next line, while the miss rate allows it
   if (num_prefs == 0 && m_spec_nl)
   {
      IntPtr pf_address = (cl_addr + 1) << LOG2_BLOCK_SIZE;
      if ((pf_address >> LOG2_PAGE_SIZE) == curr_page)
      {
         pref_addr.push_back(pf_address);
         stats.nl_prefetches++;
      }
   }

   stats.prefetches_issued += pref_addr.size();

   entry.last_cl_offset = cl_offset;
   entry.last_page = curr_page;
   updateGHB(cl_addr);

   return pref_addr;
}

UInt16 IPCPPrefetcher::updateSignature(UInt16 old_sig, SInt32 delta)
{
   // 7-bit sign-magnitude delta folded into a 12-bit signature
   SInt32 sig_delta = (delta < 0) ? (-delta + (1 << 6)) : delta;
   return ((old_sig << 1) ^ sig_delta) & 0xFFF;
}

SInt32 IPCPPrefetcher::updateConf(SInt32 stride, SInt32 pred_stride, SInt32 conf)
{
   // 2-bit saturating counter
   if (stride == pred_stride)
      return conf >= 3 ? 3 : conf + 1;
   return conf <= 0 ? 0 : conf - 1;
}

void IPCPPrefetcher::checkForStream(UInt32 index, UInt64 cl_addr)
{
   // How many of the 16 lines below / above this one are in the GHB
   UInt32 pos_count = 0, neg_count = 0;
   for (UInt32 i = 1; i <= NUM_GHB_ENTRIES; i++)
   {
      UInt64 below = cl_addr - i, above = cl_addr + i;
      for (UInt32 j = 0; j < NUM_GHB_ENTRIES; j++)
      {
         if (below == m_ghb[j])
         {
            pos_count++;
            break;
         }
      }
      for (UInt32 j = 0; j < NUM_GHB_ENTRIES; j++)
      {
         if (above == m_ghb[j])
         {
            neg_count++;
            break;
         }
      }
   }

   IPTableEntry &entry = m_ip_table[index];
   UInt32 count;
   if (pos_count > neg_count)
   {
      entry.str_dir = true;
      count = pos_count;
   }
   else
   {
      entry.str_dir = false;
      count = neg_count;
   }

   if (count > NUM_GHB_ENTRIES / 2)
   {
      entry.str_valid = true;
      if (count >= (NUM_GHB_ENTRIES * 3) / 4)
         entry.str_strength = true;
   }
   else if (!entry.str_strength)
      entry.str_valid = false;
}

void IPCPPrefetcher::updateGHB(UInt64 cl_addr)
{
   for (UInt32 i = 0; i < NUM_GHB_ENTRIES; i++)
      if (m_ghb[i] == cl_addr)
         return;
   for (UInt32 i = NUM_GHB_ENTRIES - 1; i > 0; i--)
      m_ghb[i] = m_ghb[i - 1];
   m_ghb[0] = cl_addr;
}
