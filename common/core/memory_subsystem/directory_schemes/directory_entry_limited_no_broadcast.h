#ifndef __DIRECTORY_ENTRY_LIMITED_NO_BROADCAST_H__
#define __DIRECTORY_ENTRY_LIMITED_NO_BROADCAST_H__

#include "directory_entry.h"
#include "random.h"
#include "log.h"

template <class DirectorySharers>
class DirectoryEntryLimitedNoBroadcast : public DirectoryEntrySized<DirectorySharers>
{
   public:
      DirectoryEntryLimitedNoBroadcast(UInt32 max_hw_sharers, UInt32 max_num_sharers);
      ~DirectoryEntryLimitedNoBroadcast();

      bool hasSharer(core_id_t sharer_id);
      bool addSharer(core_id_t sharer_id, UInt32 max_hw_sharers);
      void removeSharer(core_id_t sharer_id, bool reply_expected);

      core_id_t getOwner();
      void setOwner(core_id_t owner_id);

      core_id_t getOneSharer();

      SubsecondTime getLatency();

   private:
      Random m_rand_num;
};

template <class DirectorySharers>
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::DirectoryEntryLimitedNoBroadcast(
      UInt32 max_hw_sharers,
      UInt32 max_num_sharers):
   DirectoryEntrySized<DirectorySharers>(max_hw_sharers, max_num_sharers)
{}

template <class DirectorySharers>
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::~DirectoryEntryLimitedNoBroadcast()
{}

template <class DirectorySharers>
bool
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::hasSharer(core_id_t sharer_id)
{
   return this->m_sharers[sharer_id];
}

// Return value says whether the sharer was successfully added
//              'True' if it was successfully added
//              'False' if there will be an eviction before adding
template <class DirectorySharers>
bool
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::addSharer(core_id_t sharer_id, UInt32 max_hw_sharers)
{
   assert(! this->m_sharers[sharer_id]);

   if (this->getNumSharers() >= max_hw_sharers)
   {
      return false;
   }

   this->m_sharers[sharer_id] = true;
   return true;
}

template <class DirectorySharers>
void
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::removeSharer(core_id_t sharer_id, bool reply_expected)
{
   assert(!reply_expected);
   assert(this->m_sharers[sharer_id]);
   this->m_sharers[sharer_id] = false;
}

template <class DirectorySharers>
core_id_t
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::getOwner()
{
   return this->m_owner_id;
}

template <class DirectorySharers>
void
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::setOwner(core_id_t owner_id)
{
   if (owner_id != INVALID_CORE_ID)
      assert(this->m_sharers[owner_id]);
   this->m_owner_id = owner_id;
}

template <class DirectorySharers>
core_id_t
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::getOneSharer()
{
   std::pair<bool, std::vector<core_id_t> > sharers_list = this->getSharersList();
   assert(!sharers_list.first);
   assert(sharers_list.second.size() > 0);

   SInt32 index = m_rand_num.next(sharers_list.second.size());
   return sharers_list.second[index];
}

template <class DirectorySharers>
SubsecondTime
DirectoryEntryLimitedNoBroadcast<DirectorySharers>::getLatency()
{
   return SubsecondTime::Zero();
}


#endif /* __DIRECTORY_ENTRY_LIMITED_NO_BROADCAST_H__ */
