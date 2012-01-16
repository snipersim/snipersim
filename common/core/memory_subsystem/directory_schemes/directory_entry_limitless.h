#ifndef __DIRECTORY_ENTRY_LIMITLESS_H__
#define __DIRECTORY_ENTRY_LIMITLESS_H__

#include "directory_entry.h"
#include "subsecond_time.h"

template <class DirectorySharers>
class DirectoryEntryLimitless : public DirectoryEntrySized<DirectorySharers>
{
   private:
      bool m_software_trap_enabled;
      SubsecondTime m_software_trap_penalty;

   public:
      DirectoryEntryLimitless(UInt32 max_hw_sharers,
            UInt32 max_num_sharers,
            SubsecondTime software_trap_penalty);
      ~DirectoryEntryLimitless();

      bool hasSharer(core_id_t sharer_id);
      bool addSharer(core_id_t sharer_id, UInt32 max_hw_sharers);
      void removeSharer(core_id_t sharer_id, bool reply_expected);

      core_id_t getOwner();
      void setOwner(core_id_t owner_id);

      core_id_t getOneSharer();

      SubsecondTime getLatency();
};

template <class DirectorySharers>
DirectoryEntryLimitless<DirectorySharers>::DirectoryEntryLimitless(
      UInt32 max_hw_sharers,
      UInt32 max_num_sharers,
      SubsecondTime software_trap_penalty):
   DirectoryEntrySized<DirectorySharers>(max_hw_sharers, max_num_sharers),
   m_software_trap_enabled(false),
   m_software_trap_penalty(software_trap_penalty)
{}

template <class DirectorySharers>
DirectoryEntryLimitless<DirectorySharers>::~DirectoryEntryLimitless()
{}

template <class DirectorySharers>
SubsecondTime
DirectoryEntryLimitless<DirectorySharers>::getLatency()
{
   if (m_software_trap_enabled)
      return m_software_trap_penalty;
   else
      return SubsecondTime::Zero();
}

template <class DirectorySharers>
bool
DirectoryEntryLimitless<DirectorySharers>::hasSharer(core_id_t sharer_id)
{
   return this->m_sharers[sharer_id];
}

// Return value says whether the sharer was successfully added
//              'True' if it was successfully added
//              'False' if there will be an eviction before adding
template <class DirectorySharers>
bool
DirectoryEntryLimitless<DirectorySharers>::addSharer(core_id_t sharer_id, UInt32 max_hw_sharers)
{
   assert(! this->m_sharers[sharer_id]);

   // I have to calculate the latency properly here
   if (this->m_sharers.size() == max_hw_sharers)
   {
      m_software_trap_enabled = true;
   }

   this->m_sharers[sharer_id] = true;
   return true;;
}

template <class DirectorySharers>
void
DirectoryEntryLimitless<DirectorySharers>::removeSharer(core_id_t sharer_id, bool reply_expected)
{
   assert(!reply_expected);

   assert(this->m_sharers[sharer_id]);
   this->m_sharers[sharer_id] = false;
}

template <class DirectorySharers>
core_id_t
DirectoryEntryLimitless<DirectorySharers>::getOwner()
{
   return this->m_owner_id;
}

template <class DirectorySharers>
void
DirectoryEntryLimitless<DirectorySharers>::setOwner(core_id_t owner_id)
{
   if (owner_id != INVALID_CORE_ID)
      assert(this->m_sharers[owner_id]);
   this->m_owner_id = owner_id;
}

template <class DirectorySharers>
core_id_t
DirectoryEntryLimitless<DirectorySharers>::getOneSharer()
{
   core_id_t sharer_id = -1;
   for(UInt32 j = 0; j < this->m_sharers.size(); ++j) {
      if (this->m_sharers[j]) {
         sharer_id = j;
         break;
      }
   }
   assert(sharer_id != -1);
   return sharer_id;
}

#endif /* __DIRECTORY_ENTRY_LIMITLESS_H__ */
