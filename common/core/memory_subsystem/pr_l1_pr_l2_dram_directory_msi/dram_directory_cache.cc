#include "dram_directory_cache.h"
#include "log.h"
#include "utils.h"

namespace PrL1PrL2DramDirectoryMSI
{

DramDirectoryCache::DramDirectoryCache(
      core_id_t core_id,
      String directory_type_str,
      UInt32 total_entries,
      UInt32 associativity,
      UInt32 cache_block_size,
      UInt32 max_hw_sharers,
      UInt32 max_num_sharers,
      ComponentLatency dram_directory_cache_access_time,
      ShmemPerfModel* shmem_perf_model):
   m_total_entries(total_entries),
   m_associativity(associativity),
   m_cache_block_size(cache_block_size),
   m_dram_directory_cache_access_time(dram_directory_cache_access_time),
   m_shmem_perf_model(shmem_perf_model)
{
   m_num_sets = m_total_entries / m_associativity;

   // Instantiate the directory
   m_directory = new Directory(core_id, directory_type_str, total_entries, max_hw_sharers, max_num_sharers);
   m_replacement_ptrs = new UInt32[m_num_sets];

   // Logs
   m_log_num_sets = floorLog2(m_num_sets);
   m_log_cache_block_size = floorLog2(m_cache_block_size);
}

DramDirectoryCache::~DramDirectoryCache()
{
   delete m_replacement_ptrs;
   delete m_directory;
}

DirectoryEntry*
DramDirectoryCache::getDirectoryEntry(IntPtr address, bool modeled)
{
   if (m_shmem_perf_model && modeled)
      getShmemPerfModel()->incrElapsedTime(m_dram_directory_cache_access_time.getLatency(), ShmemPerfModel::_SIM_THREAD);

   IntPtr tag;
   UInt32 set_index;

   // Assume that it always hit in the Dram Directory Cache for now
   splitAddress(address, tag, set_index);

   // Find the relevant directory entry
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      DirectoryEntry* directory_entry = m_directory->getDirectoryEntry(set_index * m_associativity + i);

      if (directory_entry->getAddress() == address)
      {
         if (m_shmem_perf_model && modeled)
            getShmemPerfModel()->incrElapsedTime(directory_entry->getLatency(), ShmemPerfModel::_SIM_THREAD);
         // Simple check for now. Make sophisticated later
         return directory_entry;
      }
   }

   // Find a free directory entry if one does not currently exist
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      DirectoryEntry* directory_entry = m_directory->getDirectoryEntry(set_index * m_associativity + i);
      if (directory_entry->getAddress() == INVALID_ADDRESS)
      {
         // Simple check for now. Make sophisticated later
         directory_entry->setAddress(address);
         return directory_entry;
      }
   }

   // Check in the m_replaced_directory_entry_list
   std::vector<DirectoryEntry*>::iterator it;
   for (it = m_replaced_directory_entry_list.begin(); it != m_replaced_directory_entry_list.end(); it++)
   {
      if ((*it)->getAddress() == address)
      {
         return (*it);
      }
   }

   return (DirectoryEntry*) NULL;
}

void
DramDirectoryCache::getReplacementCandidates(IntPtr address, std::vector<DirectoryEntry*>& replacement_candidate_list)
{
   assert(getDirectoryEntry(address) == NULL);

   IntPtr tag;
   UInt32 set_index;
   splitAddress(address, tag, set_index);

   for (UInt32 i = 0; i < m_associativity; i++)
   {
      replacement_candidate_list.push_back(m_directory->getDirectoryEntry(set_index * m_associativity + ((i + m_replacement_ptrs[set_index]) % m_associativity)));
   }
   ++m_replacement_ptrs[set_index];
}

DirectoryEntry*
DramDirectoryCache::replaceDirectoryEntry(IntPtr replaced_address, IntPtr address, bool modeled)
{
   if (m_shmem_perf_model && modeled)
      getShmemPerfModel()->incrElapsedTime(m_dram_directory_cache_access_time.getLatency(), ShmemPerfModel::_SIM_THREAD);

   IntPtr tag;
   UInt32 set_index;
   splitAddress(replaced_address, tag, set_index);

   for (UInt32 i = 0; i < m_associativity; i++)
   {
      DirectoryEntry* replaced_directory_entry = m_directory->getDirectoryEntry(set_index * m_associativity + i);
      if (replaced_directory_entry->getAddress() == replaced_address)
      {
         m_replaced_directory_entry_list.push_back(replaced_directory_entry);

         DirectoryEntry* directory_entry = m_directory->createDirectoryEntry();
         directory_entry->setAddress(address);
         m_directory->setDirectoryEntry(set_index * m_associativity + i, directory_entry);

         return directory_entry;
      }
   }

   // Should not reach here
   LOG_PRINT_ERROR("");
}

void
DramDirectoryCache::invalidateDirectoryEntry(IntPtr address)
{
   std::vector<DirectoryEntry*>::iterator it;
   for (it = m_replaced_directory_entry_list.begin(); it != m_replaced_directory_entry_list.end(); it++)
   {
      if ((*it)->getAddress() == address)
      {
         delete (*it);
         m_replaced_directory_entry_list.erase(it);

         return;
      }
   }

   // Should not reach here
   LOG_PRINT_ERROR("");
}

void
DramDirectoryCache::splitAddress(IntPtr address, IntPtr& tag, UInt32& set_index)
{
   IntPtr cache_block_address = address >> getLogCacheBlockSize();
   tag = cache_block_address >> getLogNumSets();
   set_index = ((UInt32) cache_block_address) & (getNumSets() - 1);

}

}
