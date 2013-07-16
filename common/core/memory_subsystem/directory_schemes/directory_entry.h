#ifndef __DIRECTORY_ENTRY_H__
#define __DIRECTORY_ENTRY_H__

#include "fixed_types.h"
#include "directory_block_info.h"
#include "subsecond_time.h"

#include <vector>
#include <bitset>
#include <cassert>

template <long Size>
class DirectorySharersBitset : public std::bitset<Size>
{
   public:
      DirectorySharersBitset(UInt32 max_num_sharers) : std::bitset<Size>()
      { assert(max_num_sharers <= Size); }
};

class DirectorySharersVector : public std::vector<bool>
{
   public:
      DirectorySharersVector(UInt32 max_num_sharers) : std::vector<bool>(max_num_sharers, false) {}
      UInt32 count(void) const
      {
         UInt32 num_sharers = 0;
         for(UInt32 j = 0; j < this->size(); ++j) {
            if ((*this)[j]) {
               num_sharers ++;
            }
         }
         return num_sharers;
      }
};

class DirectoryEntry
{
   protected:
      IntPtr m_address;
      DirectoryBlockInfo m_directory_block_info;
      core_id_t m_owner_id;
      core_id_t m_forwarder_id;

   public:
      DirectoryEntry()
         : m_address(INVALID_ADDRESS)
         , m_directory_block_info()
         , m_owner_id(INVALID_CORE_ID)
      {}
      virtual ~DirectoryEntry()
      {}

      DirectoryBlockInfo* getDirectoryBlockInfo() { return &m_directory_block_info; }

      virtual bool hasSharer(core_id_t sharer_id) = 0;
      virtual bool addSharer(core_id_t sharer_id, UInt32 max_hw_sharers) = 0;
      virtual void removeSharer(core_id_t sharer_id, bool reply_expected = false) = 0;
      virtual UInt32 getNumSharers() = 0;

      virtual core_id_t getOwner() = 0;
      virtual void setOwner(core_id_t owner_id) = 0;

      virtual core_id_t getForwarder()
      {
         return m_forwarder_id;
      }

      virtual void setForwarder(core_id_t forwarder_id)
      {
         m_forwarder_id = forwarder_id;
      }

      IntPtr getAddress() { return m_address; }
      void setAddress(IntPtr address) { m_address = address; }

      virtual core_id_t getOneSharer() = 0;
      virtual std::pair<bool, std::vector<core_id_t> > getSharersList() = 0;

      virtual SubsecondTime getLatency() = 0;
};

template <class DirectorySharers>
class DirectoryEntrySized : public DirectoryEntry
{
   protected:
      DirectorySharers m_sharers;

   public:
      DirectoryEntrySized(UInt32 max_hw_sharers, UInt32 max_num_sharers)
         : DirectoryEntry()
         , m_sharers(max_num_sharers)
      {
      }

      virtual UInt32 getNumSharers() { return m_sharers.count(); }
      virtual std::pair<bool, std::vector<core_id_t> > getSharersList()
      {
         std::pair<bool, std::vector<core_id_t> > sharers_list;
         sharers_list.first = false;
         sharers_list.second.resize(getNumSharers());

         SInt32 i = 0;
         for(UInt32 j = 0; j < m_sharers.size(); ++j) {
            if (m_sharers[j]) {
               core_id_t new_sharer = j;
               sharers_list.second[i] = new_sharer;
               i++;
               assert (i <= (core_id_t) m_sharers.size());
            }
         }

         return sharers_list;
      }
};

#endif /* __DIRECTORY_ENTRY_H__ */
