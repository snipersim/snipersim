#ifndef __DIRECTORY_H__
#define __DIRECTORY_H__

#include "directory_entry.h"
#include "fixed_types.h"
#include "subsecond_time.h"

class Directory
{
   public:
      enum DirectoryType
      {
         FULL_MAP = 0,
         LIMITED_NO_BROADCAST,
         LIMITLESS,
         NUM_DIRECTORY_TYPES
      };

   private:
      DirectoryType m_directory_type;
      UInt32 m_num_entries;
      UInt64 m_num_entries_allocated;
      UInt32 m_max_hw_sharers;
      UInt32 m_use_max_hw_sharers;
      UInt32 m_max_num_sharers;

      // FIXME: Hack: Get me out of here
      SubsecondTime m_limitless_software_trap_penalty;

      DirectoryEntry** m_directory_entry_list;

   public:
      Directory(core_id_t core_id, String directory_type_str, UInt32 num_entries, UInt32 max_hw_sharers, UInt32 max_num_sharers);
      ~Directory();

      DirectoryEntry* getDirectoryEntry(UInt32 entry_num);
      void setDirectoryEntry(UInt32 entry_num, DirectoryEntry* directory_entry);
      DirectoryEntry* createDirectoryEntry();
      template <class DirectorySharers> DirectoryEntry* createDirectoryEntrySized();

      UInt32 getMaxHwSharers() const { return m_use_max_hw_sharers; }

      static DirectoryType parseDirectoryType(String directory_type_str);
};

#endif /* __DIRECTORY_H__ */
