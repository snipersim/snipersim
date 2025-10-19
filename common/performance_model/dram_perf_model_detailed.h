#ifndef __DRAM_PERF_MODEL_DETAILED_H__
#define __DRAM_PERF_MODEL_DETAILED_H__

#include "dram_perf_model.h"
#include "queue_model.h"
#include "fixed_types.h"
#include "subsecond_time.h"
#include "dram_cntlr_interface.h"
#include "address_home_lookup.h"

#include <vector>
#include <bitset>
#include <map>
#include <list>
#include <algorithm>
#include <queue>
// We added this performance model
class DramPerfModelDetailed : public DramPerfModel
{
private:
    const core_id_t m_processor_id;
    const AddressHomeLookup *m_addr_home_locator;
    // ParametricDramDirectoryMSI::MemoryManager * m_memory_manager; // Link to MMU
    const UInt32 m_bank_count_per_rank;
    const UInt32 m_bank_count_per_rank_log2;
    const UInt32 m_bank_group_count;
    const UInt32 m_rank_count_per_channel;
    const UInt32 m_rank_bit_offset;
    const UInt32 m_channel_count;
    const UInt32 m_channel_bit_offset;
    const UInt32 m_home_locator_bit;
    const UInt32 m_aggregate_ranks;
    const UInt32 m_banks_per_channel_count;
    const UInt32 m_banks_per_group_count;
    const UInt32 m_aggregate_banks;
    const UInt32 m_aggregate_bank_groups;
    const UInt32 m_bus_bit_width;
    const UInt32 m_dram_frequency_mhz;
    const UInt32 m_row_buffer_size_bytes;
    const UInt32 m_row_buffer_size_bytes_log2;
    const bool m_use_open_row_policy;
    const UInt32 m_col_bit_offset;
    const UInt32 m_col_high_bit_offset;
    const UInt32 m_bank_bit_offset;
    const bool m_enable_address_randomization;
    const UInt32 m_randomization_bit_offset;
    const UInt32 m_col_bits_position;
    const ComponentBandwidth m_data_bus_throughput;
    const SubsecondTime m_row_open_duration;
    const SubsecondTime m_row_activation_latency;
    const SubsecondTime m_row_precharge_latency;
    const SubsecondTime m_base_access_latency;
    const SubsecondTime m_cmd_to_cmd_delay;
    const SubsecondTime m_short_cmd_delay;
    const SubsecondTime m_long_cmd_delay;
    const SubsecondTime m_mem_ctrl_latency;
    const SubsecondTime m_refresh_cycle_period;
    const SubsecondTime m_refresh_cycle_duration;

    const UInt32 m_local_dram_capacity_threshold;

    std::vector<QueueModel *> m_request_queue_models;
    std::vector<QueueModel *> m_rank_availability_trackers;
    std::vector<QueueModel *> m_bank_group_availability_trackers;

    // I want to represent DRAM availability intervals using a tree structure
    // There is a tree for each bank in the system and each tree has a node for each interval of time that the bank is busy

    typedef enum
    {
        METADATA,
        NOT_METADATA,
        NUMBER_OF_TYPES
    } page_type;

    struct IntervalNode
    {
        SubsecondTime start_time; // Start of the interval
        SubsecondTime end_time;   // End of the interval
        IntPtr open_page;         // Page that is open during this interval

        // Explicit constructor to ensure all members are initialized
        // Provide default arguments for convenience.
        IntervalNode(SubsecondTime s_time = SubsecondTime::Zero(),
                     SubsecondTime e_time = SubsecondTime::Zero(),
                     IntPtr o_page = static_cast<IntPtr>(-1))
            : start_time(s_time),
              end_time(e_time),
              open_page(o_page)
        {
            // Any additional initialization if needed
        }

        // Priority: based on interval length (example)
        bool operator<(const IntervalNode &node) const
        {
            return (start_time) > (node.start_time); // This is a min-heap (smallest start_time at top)
        }
    };

    struct BankState
    {
        core_id_t owner_core;
        IntPtr active_row_address;
        SubsecondTime next_available_time;
        SubsecondTime peak_contention_time;
        IntPtr peak_contention_page;
        page_type active_row_category;
        std::priority_queue<IntervalNode> m_bank_activity_schedule;
    };

    std::vector<BankState> m_bank_state_info;

    UInt64 m_stat_row_hits;
    UInt64 m_stat_row_empty_accesses;
    UInt64 m_stat_row_closures;
    UInt64 m_stat_row_misses;
    UInt64 m_stat_row_conflict_metadata_to_data;
    UInt64 m_stat_row_conflict_data_to_metadata;
    UInt64 m_stat_row_conflict_data_to_data;
    UInt64 m_stat_row_conflict_metadata_to_metadata;

    UInt64 m_stat_past_requests;
    UInt64 m_stat_unknown_past_requests;
    UInt64 m_stat_present_requests;

    UInt32 m_needed_bank_bits;
    UInt32 m_needed_rank_bits;
    UInt32 m_needed_channel_bits;
    UInt32 m_dram_row_size_bits;

    SubsecondTime m_cumulative_queue_latency;
    SubsecondTime m_cumulative_access_latency;

    bool is_fixed_latency_policy;
    bool is_selective_fixed_latency_policy;
    bool is_open_row_buffer_policy;

    void parseDeviceAddress(IntPtr address, UInt32 &channel, UInt32 &rank, UInt32 &bank_group, UInt32 &bank, UInt32 &column, UInt64 &page);
    UInt64 parseAddressBits(UInt64 address, UInt32 &data, UInt32 offset, UInt32 size, UInt64 base_address);

    std::pair<SubsecondTime, IntervalNode> fallsWithinInterval(UInt64 page, SubsecondTime pkt_time, IntPtr bank);
    void cleanupBusyIntervals(IntPtr bank);
    void printInterval(std::priority_queue<IntervalNode> intervals);

public:
    DramPerfModelDetailed(core_id_t core_id, UInt32 cache_block_size, AddressHomeLookup *address_home_lookup);

    ~DramPerfModelDetailed();

    SubsecondTime getAccessLatencyUnmodelled(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address);
    SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address, DramCntlrInterface::access_t access_type, ShmemPerf *perf, bool is_metadata);
};

#endif /* __DRAM_PERF_MODEL_Detailed_H__ */
