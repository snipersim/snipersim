
#pragma once
#include "fixed_types.h"
#include "cache.h"
#include "simulator.h"
#include "core.h"
#include "core_manager.h"
#include <unordered_map>
#include <mutex>
#include <random>
#include <cstdint>
#include "pwc.h"
#include <bitset>

// #define DEBUG
#include "payload_word.h"
using namespace std;

namespace ParametricDramDirectoryMSI
{
	/**
	 * @brief Represents a single memory access during a page table walk.
	 * 
	 * Each PTWAccess captures the details of accessing one page table entry
	 * during the translation process.
	 */
	struct PTWAccess
	{
		int table_level;       ///< Level of page table based on page size (e.g., 0=4KB, 1=2MB, 2=1GB)
		int depth;             ///< Depth in the page table hierarchy (e.g., PML4=0, PDP=1, PD=2, PT=3)
		IntPtr physical_addr;  ///< Physical address of the page table entry accessed
		bool is_pte;           ///< True if this is the final PTE containing the translation

		PTWAccess() : table_level(0), depth(0), physical_addr(0), is_pte(false) {}
		
		PTWAccess(int table_level, int depth, IntPtr physical_addr, bool is_pte)
			: table_level(table_level), depth(depth), physical_addr(physical_addr), is_pte(is_pte) {}
		
		// Comparison operators for sorting and uniqueness
		bool operator<(const PTWAccess& other) const {
			if (table_level != other.table_level) return table_level < other.table_level;
			if (depth != other.depth) return depth < other.depth;
			if (physical_addr != other.physical_addr) return physical_addr < other.physical_addr;
			return is_pte < other.is_pte;
		}
		
		bool operator==(const PTWAccess& other) const {
			return table_level == other.table_level && depth == other.depth && 
			       physical_addr == other.physical_addr && is_pte == other.is_pte;
		}
	};

	/// Vector of page table accesses made during a walk
	typedef std::vector<PTWAccess> accessedAddresses;

	/**
	 * @brief Result of a page table walk operation.
	 * 
	 * Encapsulates all information returned from walking the page table,
	 * including the translation result, accesses made, and any faults.
	 */
	struct PTWResult
	{
		int page_size;                  ///< Page size in bits (e.g., 12=4KB, 21=2MB, 30=1GB)
		accessedAddresses accesses;     ///< All page table entries accessed during the walk
		IntPtr ppn;                     ///< Physical page number (result of translation)
		SubsecondTime pwc_latency;      ///< Page walk cache latency for intermediate levels
		bool fault_happened;            ///< True if a page fault occurred during the walk
		int requested_frames;           ///< Number of frames needed for page fault (PT frames + data frame)
		PayloadWord payload_bits;       ///< Shadow PTE payload (temporal offset entries for prefetching)

		PTWResult() 
			: page_size(0), accesses(), ppn(0), pwc_latency(SubsecondTime::Zero()), fault_happened(false), requested_frames(0), payload_bits(0) {}
		
		PTWResult(int page_size, const accessedAddresses& accesses, IntPtr ppn, 
		          SubsecondTime pwc_latency, bool fault_happened, int requested_frames = 0)
			: page_size(page_size), accesses(accesses), ppn(ppn), 
			  pwc_latency(pwc_latency), fault_happened(fault_happened), requested_frames(requested_frames), payload_bits(0) {}

		// For backward compatibility with tuple-based code using get<N>()
		template<std::size_t I>
		auto& get() {
			if constexpr (I == 0) return page_size;
			else if constexpr (I == 1) return accesses;
			else if constexpr (I == 2) return ppn;
			else if constexpr (I == 3) return pwc_latency;
			else if constexpr (I == 4) return fault_happened;
		}
		
		template<std::size_t I>
		const auto& get() const {
			if constexpr (I == 0) return page_size;
			else if constexpr (I == 1) return accesses;
			else if constexpr (I == 2) return ppn;
			else if constexpr (I == 3) return pwc_latency;
			else if constexpr (I == 4) return fault_happened;
		}
	};

	typedef int PageSize;

	class PageTable
	{

	protected:
		int core_id;
		String name;
		String type;
		int *m_page_size_list;
		int m_page_sizes;
		Core *core;
		bool is_guest;

		// This is used to track the number of accesses per virtual page number (VPN).
		// One page table serves every core of its application, and their walks
		// run concurrently (a simulator thread per core): the maps below are
		// reached only under m_maps_lock (countVPNAccess, setVPNAccesses and the
		// payload calls).  An insertion that rehashed under another core's
		// increment crashed the simulator (SIGSEGV in initializeWalk).
		std::unordered_map<IntPtr, UInt64> accesses_per_vpn;
		mutable std::mutex m_maps_lock;

		void countVPNAccess(IntPtr vpn)
		{
			std::lock_guard<std::mutex> lock(m_maps_lock);
			accesses_per_vpn[vpn]++;
		}

		void setVPNAccesses(IntPtr vpn, UInt64 n)
		{
			std::lock_guard<std::mutex> lock(m_maps_lock);
			accesses_per_vpn[vpn] = n;
		}

		// Shadow PTE payload: stores per-VPN temporal offset data for the
		// TLB prefetchers.  Keyed by VPN (address >> 12).
		std::unordered_map<uint64_t, PayloadWord> shadow_pte_payload;

	public:
		PageTable(int core_id, String name, String type, int page_sizes, int *page_size_list, bool is_guest = false)
		{
			this->m_page_sizes = page_sizes;
			this->m_page_size_list = page_size_list;
			this->core_id = core_id;
			this->name = name;
			this->is_guest = is_guest;
			this->type = type;
		};

		virtual PTWResult initializeWalk(IntPtr address, bool count, bool is_prefetch = false, bool restart_walk = false) = 0;
		int *getPageSizes() { return m_page_size_list; };
		int getPageSizesCount() { return m_page_sizes; };
		virtual int getMaxLevel() { return -1; }; // This function should be overriden by the derived class (e.g., in RadixPageTable there are maximum 4 levels)
		String getName() { return name; };
		String getType() { return type; };
		bool isGuest() const { return is_guest; }; // A guest (VM) page table: its frames come from the guest OS
		virtual void deletePage(IntPtr address) {};
		virtual int updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames) = 0;

		// ----------------------------------------------------------------
		// Shadow PTE payload API  (used by TLB prefetchers)
		// ----------------------------------------------------------------

		/** Read the shadow payload word for a given VPN. Returns 0 if none stored. */
		PayloadWord readPayloadBits(uint64_t vpn) const
		{
			std::lock_guard<std::mutex> lock(m_maps_lock);
			auto it = shadow_pte_payload.find(vpn);
			return (it != shadow_pte_payload.end()) ? it->second : PayloadWord();
		}

		/** Write (overwrite) the shadow payload word for a given VPN. */
		void writePayloadBits(uint64_t vpn, const PayloadWord& payload)
		{
			std::lock_guard<std::mutex> lock(m_maps_lock);
			shadow_pte_payload[vpn] = payload;
		}

		// ----------------------------------------------------------------
		// Shadow PMD payload: the 2MB plane, keyed by address >> 21.
		// Replaces fillPayloadBits, which had no caller and truncated a
		// 128-bit payload into a 64-bit PTWResult field on the way past.
		// ----------------------------------------------------------------
		std::unordered_map<uint64_t, PayloadWord> shadow_pmd_payload;

		PayloadWord readPMDPayloadBits(uint64_t vpn_2mb) const
		{
			std::lock_guard<std::mutex> lock(m_maps_lock);
			auto it = shadow_pmd_payload.find(vpn_2mb);
			return (it != shadow_pmd_payload.end()) ? it->second : PayloadWord();
		}

		void writePMDPayloadBits(uint64_t vpn_2mb, const PayloadWord& payload)
		{
			std::lock_guard<std::mutex> lock(m_maps_lock);
			shadow_pmd_payload[vpn_2mb] = payload;
		}
		UInt64 getAccessesPerVPN(IntPtr vpn)
		{
			std::lock_guard<std::mutex> lock(m_maps_lock);
			auto it = accesses_per_vpn.find(vpn);
			return it != accesses_per_vpn.end() ? it->second : 0;
		};
	};
}
