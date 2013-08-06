#include "simulator.h"
#include "directory.h"
#include "directory_entry.h"
#include "directory_entry_limited_no_broadcast.h"
#include "directory_entry_limitless.h"
#include "stats.h"
#include "log.h"
#include "config.hpp"

Directory::Directory(core_id_t core_id, String directory_type_str, UInt32 num_entries, UInt32 max_hw_sharers, UInt32 max_num_sharers):
   m_num_entries(num_entries),
   m_num_entries_allocated(0),
   m_max_hw_sharers(max_hw_sharers),
   m_use_max_hw_sharers(max_hw_sharers), // Value to pass through to DirectoryEntry::addSharer
   m_max_num_sharers(max_num_sharers),
   m_limitless_software_trap_penalty(SubsecondTime::Zero())
{
   // Look at the type of directory and create
   m_directory_entry_list = new DirectoryEntry*[m_num_entries];

   m_directory_type = parseDirectoryType(directory_type_str);
   for (UInt32 i = 0; i < m_num_entries; i++)
   {
      m_directory_entry_list[i] = NULL;
   }

   if (m_directory_type == LIMITLESS)
   {
      try
      {
         m_limitless_software_trap_penalty = SubsecondTime::NS() * Sim()->getCfg()->getInt("perf_model/dram_directory/limitless/software_trap_penalty");
      }
      catch(...)
      {
         LOG_PRINT_ERROR("Could not read 'cache_coherence/limitless/software_trap_penalty' from the config file");
      }
   }

   registerStatsMetric("directory", core_id, "entries-allocated", &m_num_entries_allocated);
}

Directory::~Directory()
{
   for (UInt32 i = 0; i < m_num_entries; i++)
   {
      if (m_directory_entry_list[i])
         delete m_directory_entry_list[i];
   }
   delete [] m_directory_entry_list;
}

DirectoryEntry*
Directory::getDirectoryEntry(UInt32 entry_num)
{
   LOG_ASSERT_ERROR(entry_num < m_num_entries, "Invalid entry_num(%d) >= num_entries(%d)", entry_num, m_num_entries);

   if (m_directory_entry_list[entry_num] == NULL)
   {
      m_directory_entry_list[entry_num] = createDirectoryEntry();
      ++m_num_entries_allocated;
   }
   return m_directory_entry_list[entry_num];
}

void
Directory::setDirectoryEntry(UInt32 entry_num, DirectoryEntry* directory_entry)
{
   m_directory_entry_list[entry_num] = directory_entry;
}

Directory::DirectoryType
Directory::parseDirectoryType(String directory_type_str)
{
   if (directory_type_str == "full_map")
      return FULL_MAP;
   else if (directory_type_str == "limited_no_broadcast")
      return LIMITED_NO_BROADCAST;
   else if (directory_type_str == "limitless")
      return LIMITLESS;
   else
   {
      LOG_PRINT_ERROR("Unsupported Directory Type: %s", directory_type_str.c_str());
      return (DirectoryType) -1;
   }
}

DirectoryEntry*
Directory::createDirectoryEntry()
{
   // Specify the storage class to use for counting the directory sharers.
   // Due to alignment issues, the minimum size can already hold up to 64 nodes.
   if (m_max_num_sharers <= 64)
      return createDirectoryEntrySized<DirectorySharersBitset<64> >();
   else if (m_max_num_sharers <= 128)
      return createDirectoryEntrySized<DirectorySharersBitset<128> >();
   else if (m_max_num_sharers <= 256)
      return createDirectoryEntrySized<DirectorySharersBitset<256> >();
   else if (m_max_num_sharers <= 1024)
      return createDirectoryEntrySized<DirectorySharersBitset<1024> >();
   else
      return createDirectoryEntrySized<DirectorySharersVector>();
}

template <class DirectorySharers>
DirectoryEntry*
Directory::createDirectoryEntrySized()
{
   switch (m_directory_type)
   {
      case FULL_MAP:
         m_use_max_hw_sharers = m_max_num_sharers;
         return new DirectoryEntryLimitedNoBroadcast<DirectorySharers>(m_max_num_sharers, m_max_num_sharers);

      case LIMITED_NO_BROADCAST:
         return new DirectoryEntryLimitedNoBroadcast<DirectorySharers>(m_max_hw_sharers, m_max_num_sharers);

      case LIMITLESS:
         return new DirectoryEntryLimitless<DirectorySharers>(m_max_hw_sharers, m_max_num_sharers, m_limitless_software_trap_penalty);

      default:
         LOG_PRINT_ERROR("Unrecognized Directory Type: %u", m_directory_type);
         return NULL;
   }
}
