#include "cwc.h"
#include "simulator.h"
#include "stats.h"

namespace ParametricDramDirectoryMSI
{
	CWCache::CWCache(String _name, Core *_core, int _size)
		: name(_name), core(_core), size(_size)
	{
		m_map.reserve(_size);

		cwc_stats.cwc_hits = 0;
		cwc_stats.cwc_accesses = 0;
		registerStatsMetric(name, core->getId(), "cwc_hits", &cwc_stats.cwc_hits);
		registerStatsMetric(name, core->getId(), "cwc_accesses", &cwc_stats.cwc_accesses);
	}

	bool CWCache::lookup(IntPtr tag, CWCRow &entry)
	{
		cwc_stats.cwc_accesses++;

		auto it = m_map.find(tag);
		if (it == m_map.end())
			return false;
		cwc_stats.cwc_hits++;

		// Most recently used
		m_lru_list.splice(m_lru_list.begin(), m_lru_list, it->second);
		entry = *it->second;
		return true;
	}

	void CWCache::insert(const CWCRow &entry)
	{
		auto it = m_map.find(entry.cwt_entry_ptr->tag);
		if (it != m_map.end())
		{
			*it->second = entry;
			m_lru_list.splice(m_lru_list.begin(), m_lru_list, it->second);
			return;
		}

		if (static_cast<int>(m_lru_list.size()) == size)
		{
			IntPtr lru_tag = m_lru_list.back().cwt_entry_ptr->tag;
			m_lru_list.pop_back();
			m_map.erase(lru_tag);
		}

		m_lru_list.push_front(entry);
		m_map[entry.cwt_entry_ptr->tag] = m_lru_list.begin();
	}
}
