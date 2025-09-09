#include "dram_perf_model_detailed.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "stats.h"
#include "shmem_perf.h"
#include "subsecond_time.h"
#include "utils.h"

// #define DEBUG_PRINT
// We added this performance model

DramPerfModelDetailed::DramPerfModelDetailed(core_id_t core_id, UInt32 cache_block_size, AddressHomeLookup *address_home_lookup)
    : DramPerfModel(core_id, cache_block_size), m_processor_id(core_id), m_addr_home_locator(address_home_lookup), m_bank_count_per_rank(Sim()->getCfg()->getInt("perf_model/dram/ddr/num_banks")), m_bank_count_per_rank_log2(floorLog2(m_bank_count_per_rank)), m_bank_group_count(Sim()->getCfg()->getInt("perf_model/dram/ddr/num_bank_groups")), m_rank_count_per_channel(Sim()->getCfg()->getInt("perf_model/dram/ddr/num_ranks")), m_rank_bit_offset(Sim()->getCfg()->getInt("perf_model/dram/ddr/rank_offset")), m_channel_count(Sim()->getCfg()->getInt("perf_model/dram/ddr/num_channels")), m_channel_bit_offset(Sim()->getCfg()->getInt("perf_model/dram/ddr/channel_offset")), m_home_locator_bit(Sim()->getCfg()->getInt("perf_model/dram_directory/home_lookup_param")), m_aggregate_ranks(m_rank_count_per_channel * m_channel_count), m_banks_per_channel_count(m_bank_count_per_rank * m_rank_count_per_channel), m_banks_per_group_count(m_bank_count_per_rank / m_bank_group_count), m_aggregate_banks(m_banks_per_channel_count * m_channel_count), m_aggregate_bank_groups(m_bank_group_count * m_rank_count_per_channel * m_channel_count), m_bus_bit_width(Sim()->getCfg()->getInt("perf_model/dram/ddr/data_bus_width")) // In bits
      ,
      m_dram_frequency_mhz(Sim()->getCfg()->getInt("perf_model/dram/ddr/dram_speed")) // In MHz
      ,
      m_row_buffer_size_bytes(Sim()->getCfg()->getInt("perf_model/dram/ddr/dram_page_size")), m_row_buffer_size_bytes_log2(floorLog2(m_row_buffer_size_bytes)), m_use_open_row_policy(Sim()->getCfg()->getBool("perf_model/dram/ddr/open_page_mapping")), m_col_bit_offset(Sim()->getCfg()->getInt("perf_model/dram/ddr/column_offset")), m_col_high_bit_offset(m_row_buffer_size_bytes_log2 - m_col_bit_offset + m_bank_count_per_rank_log2) // Offset for higher order column bits
      ,
      m_bank_bit_offset(m_row_buffer_size_bytes_log2 - m_col_bit_offset) // Offset for bank bits
      ,
      m_enable_address_randomization(Sim()->getCfg()->getBool("perf_model/dram/ddr/randomize_address")), m_randomization_bit_offset(Sim()->getCfg()->getInt("perf_model/dram/ddr/randomize_offset")), m_col_bits_position(Sim()->getCfg()->getInt("perf_model/dram/ddr/column_bits_shift")), m_data_bus_throughput(m_dram_frequency_mhz * m_bus_bit_width / 1000) // In bits/ns: MT/s=transfers/us * bits/transfer
      ,
      m_row_open_duration(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_keep_open"))), m_row_activation_latency(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_open_delay"))), m_row_precharge_latency(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_close_delay"))), m_base_access_latency(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/access_cost"))), m_cmd_to_cmd_delay(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay"))), m_short_cmd_delay(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay_short"))), m_long_cmd_delay(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay_long"))), m_mem_ctrl_latency(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/controller_delay"))), m_refresh_cycle_period(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/refresh_interval"))), m_refresh_cycle_duration(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/refresh_length"))), m_local_dram_capacity_threshold(Sim()->getCfg()->getInt("perf_model/dram/localdram_size")) // Move data if greater than
      ,
      m_bank_state_info(m_aggregate_banks), m_stat_row_hits(0), m_stat_row_empty_accesses(0), m_stat_row_closures(0), m_stat_row_misses(0), m_stat_row_conflict_metadata_to_data(0), m_stat_row_conflict_data_to_metadata(0), m_stat_row_conflict_data_to_data(0), m_stat_row_conflict_metadata_to_metadata(0), m_stat_past_requests(0), m_stat_unknown_past_requests(0), m_stat_present_requests(0), m_cumulative_queue_latency(SubsecondTime::Zero()), m_cumulative_access_latency(SubsecondTime::Zero()), is_fixed_latency_policy(Sim()->getCfg()->getBool("perf_model/dram/ddr/constant_time_policy")), is_selective_fixed_latency_policy(Sim()->getCfg()->getBool("perf_model/dram/ddr/selective_constant_time_policy")), is_open_row_buffer_policy(Sim()->getCfg()->getBool("perf_model/dram/ddr/open_row_policy")), m_needed_bank_bits(Sim()->getCfg()->getInt("perf_model/dram/ddr/required_bank_bits")), m_needed_rank_bits(Sim()->getCfg()->getInt("perf_model/dram/ddr/required_rank_bits")), m_needed_channel_bits(Sim()->getCfg()->getInt("perf_model/dram/ddr/required_channel_bits")), m_dram_row_size_bits(Sim()->getCfg()->getInt("perf_model/dram/ddr/dram_page_size_bits"))
{

    String model_name("dram");
    if (Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled"))
    {
        for (UInt32 chan_idx = 0; chan_idx < m_channel_count; ++chan_idx)
        {
            m_request_queue_models.push_back(QueueModel::create(
                model_name + "-queue-" + itostr(chan_idx), core_id, Sim()->getCfg()->getString("perf_model/dram/queue_model/type"),
                m_data_bus_throughput.getRoundedLatency(8))); // bytes to bits
        }
    }

    registerStatsMetric("dram", core_id, "total-access-latency", &m_cumulative_access_latency);
    for (UInt32 rank_idx = 0; rank_idx < m_aggregate_ranks; ++rank_idx)
    {
        m_rank_availability_trackers.push_back(QueueModel::create(
            model_name + "-rank-" + itostr(rank_idx), core_id, "history_list",
            (m_bank_group_count > 1) ? m_short_cmd_delay : m_cmd_to_cmd_delay));
    }

    for (UInt32 group_idx = 0; group_idx < m_aggregate_bank_groups; ++group_idx)
    {
        m_bank_group_availability_trackers.push_back(QueueModel::create(
            model_name + "-bank-group-" + itostr(group_idx), core_id, "history_list",
            m_long_cmd_delay));
    }

    for (UInt32 bank_idx = 0; bank_idx < m_aggregate_banks; ++bank_idx)
    {
        m_bank_state_info[bank_idx].owner_core = -1;
        m_bank_state_info[bank_idx].active_row_address = -1;
        m_bank_state_info[bank_idx].next_available_time = SubsecondTime::Zero();
        m_bank_state_info[bank_idx].peak_contention_time = SubsecondTime::Zero();
        m_bank_state_info[bank_idx].peak_contention_page = -1;
        m_bank_state_info[bank_idx].active_row_category = NOT_METADATA;
    }

    LOG_ASSERT_ERROR(cache_block_size == 64, "Hardcoded for 64-byte cache lines");
    LOG_ASSERT_ERROR(m_col_bit_offset <= m_row_buffer_size_bytes_log2, "Column offset exceeds bounds!");
    if (m_enable_address_randomization)
        LOG_ASSERT_ERROR(m_bank_group_count == 4 || m_bank_group_count == 8, "Number of bank groups incorrect for address randomization!");

    registerStatsMetric("ddr", core_id, "page-hits", &m_stat_row_hits);
    registerStatsMetric("ddr", core_id, "page-empty", &m_stat_row_empty_accesses);
    registerStatsMetric("ddr", core_id, "page-closing", &m_stat_row_closures);
    registerStatsMetric("ddr", core_id, "page-miss", &m_stat_row_misses);
    registerStatsMetric("ddr", core_id, "page-conflict-data-to-metadata", &m_stat_row_conflict_data_to_metadata);
    registerStatsMetric("ddr", core_id, "page-conflict-metadata-to-data", &m_stat_row_conflict_metadata_to_data);
    registerStatsMetric("ddr", core_id, "page-conflict-metadata-to-metadata", &m_stat_row_conflict_metadata_to_metadata);
    registerStatsMetric("ddr", core_id, "page-conflict-data-to-data", &m_stat_row_conflict_data_to_data);

    registerStatsMetric("dram", core_id, "received-request-from-the-past", &m_stat_past_requests);
    registerStatsMetric("dram", core_id, "received-request-from-the-unknown-past", &m_stat_unknown_past_requests);
    registerStatsMetric("dram", core_id, "received-request-from-present", &m_stat_present_requests);
}

DramPerfModelDetailed::~DramPerfModelDetailed()
{

    if (m_request_queue_models.size())
    {
        for (UInt32 chan_idx = 0; chan_idx < m_channel_count; ++chan_idx)
            delete m_request_queue_models[chan_idx];
    }
    if (m_rank_availability_trackers.size())
    {
        for (UInt32 rank_idx = 0; rank_idx < m_aggregate_ranks; ++rank_idx)
            delete m_rank_availability_trackers[rank_idx];
    }

    if (m_bank_group_availability_trackers.size())
    {
        for (UInt32 group_idx = 0; group_idx < m_aggregate_bank_groups; ++group_idx)
            delete m_bank_group_availability_trackers[group_idx];
    }
}

UInt64
DramPerfModelDetailed::parseAddressBits(UInt64 address, UInt32 &data, UInt32 offset, UInt32 size, UInt64 base_address = 0)
{
    // parse data from the address based on the offset and size, return the address without the bits used to parse the data.
    UInt32 size_log2 = floorLog2(size);
    if (base_address != 0)
    {
        data = (base_address >> offset) % size;
    }
    else
    {
        data = (address >> offset) % size;
    }
    return ((address >> (offset + size_log2)) << offset) | (address & ((1 << offset) - 1));
}

void DramPerfModelDetailed::parseDeviceAddress(IntPtr address, UInt32 &out_channel, UInt32 &out_rank, UInt32 &out_bank_group, UInt32 &out_bank, UInt32 &out_column, UInt64 &out_page)
{
    // Construct DDR address which has bits used for interleaving removed
    UInt64 flat_address = m_addr_home_locator->getLinearAddress(address);
    UInt64 relevant_address_bits = flat_address >> 6;

#ifdef DEBUG_PRINT
    std::cout << "Address bits: " << std::bitset<64>(relevant_address_bits) << std::endl;
#endif

    if (m_use_open_row_policy)
    {
        // Open-page mapping: column address is bottom bits, then bank, then page
        if (m_col_bit_offset)
        {
            // Column address is split into 2 halves ColHi and ColLo and
            // the address looks like: | Page | ColHi | Bank | ColLo |
            // m_col_bit_offset specifies the number of ColHi bits
            out_column = (((relevant_address_bits >> m_col_high_bit_offset) << m_bank_bit_offset) | (relevant_address_bits & ((1 << m_bank_bit_offset) - 1))) % m_row_buffer_size_bytes;
            relevant_address_bits = relevant_address_bits >> m_bank_bit_offset;
            out_bank_group = relevant_address_bits % m_bank_group_count;
            out_bank = relevant_address_bits % m_bank_count_per_rank;
            relevant_address_bits = relevant_address_bits >> (m_bank_count_per_rank_log2 + m_col_bit_offset);
        }
        else
        {
            out_channel = relevant_address_bits % m_channel_count;
            relevant_address_bits = relevant_address_bits >> m_needed_channel_bits;

#ifdef DEBUG_PRINT
            std::cout << "Channel: " << out_channel << std::endl;
#endif
            out_column = relevant_address_bits % m_row_buffer_size_bytes;
            relevant_address_bits = relevant_address_bits >> m_dram_row_size_bits;

#ifdef DEBUG_PRINT
            std::cout << "Column: " << out_column << std::endl;
#endif

            out_rank = relevant_address_bits % m_rank_count_per_channel;
            relevant_address_bits = relevant_address_bits >> m_needed_rank_bits;

#ifdef DEBUG_PRINT
            std::cout << "Rank: " << out_rank << std::endl;
#endif

            out_bank_group = relevant_address_bits % m_bank_group_count;

#ifdef DEBUG_PRINT
            std::cout << "Bank Group: " << out_bank_group << std::endl;
#endif

            out_bank = relevant_address_bits % m_bank_count_per_rank;
            relevant_address_bits = relevant_address_bits >> m_needed_bank_bits;

#ifdef DEBUG_PRINT
            std::cout << "Bank: " << out_bank << std::endl;
#endif
        }
        out_page = relevant_address_bits;

#ifdef DEBUG_PRINT
        std::cout << "Page: " << out_page << std::endl;
#endif
    }
    else
    {
        out_bank_group = relevant_address_bits % m_bank_group_count;
        out_bank = relevant_address_bits % m_bank_count_per_rank;
        relevant_address_bits /= m_bank_count_per_rank;

        // Closed-page mapping: column address is bits X+banksize:X, row address is everything else
        // (from whatever is left after cutting channel/rank/bank from the bottom)
        out_column = (relevant_address_bits >> m_col_bits_position) % m_row_buffer_size_bytes;
        out_page = (((relevant_address_bits >> m_col_bits_position) / m_row_buffer_size_bytes) << m_col_bits_position) | (relevant_address_bits & ((1 << m_col_bits_position) - 1));
    }
}

std::pair<SubsecondTime, DramPerfModelDetailed::IntervalNode> DramPerfModelDetailed::fallsWithinInterval(UInt64 page, SubsecondTime pkt_time, IntPtr bank_idx)
{
    if (m_bank_state_info[bank_idx].m_bank_activity_schedule.empty())
    {
        // No intervals, bank is immediately available
        return {pkt_time, IntervalNode(SubsecondTime::Zero(), SubsecondTime::Zero(), -1)};
    }

    std::priority_queue<IntervalNode> intervals_copy = m_bank_state_info[bank_idx].m_bank_activity_schedule;
    IntervalNode preceding_interval;
    preceding_interval.start_time = SubsecondTime::Zero();
    preceding_interval.end_time = SubsecondTime::Zero();
    preceding_interval.open_page = -1;

    while (!intervals_copy.empty())
    {
        IntervalNode current_interval = intervals_copy.top();
        intervals_copy.pop();

        if (current_interval.start_time > pkt_time)
        {
            if (preceding_interval.open_page == -1)
            {
                m_stat_unknown_past_requests++;
#ifdef DEBUG_PRINT
                std::cout << "DRAM received request from the unknown past Counter: " << m_stat_unknown_past_requests << std::endl;
                std::cout << "pkt_time: " << pkt_time.getNS() << " current_interval.start_time: " << current_interval.start_time.getNS() << " current_interval.end_time: " << current_interval.end_time.getNS() << "max_time: " << m_bank_state_info[bank_idx].peak_contention_time.getNS() << std::endl;
#endif
            }
            else
            {
                m_stat_past_requests++;
#ifdef DEBUG_PRINT
                std::cout << "DRAM received request from the past Counter: " << m_stat_past_requests << std::endl;
#endif
            }
            // Found an interval that starts after pkt_time
            return {pkt_time, preceding_interval}; // Return pkt_time as the available cycle
        }

        if (current_interval.start_time <= pkt_time && current_interval.end_time >= pkt_time)
        {
            m_stat_past_requests++;
#ifdef DEBUG_PRINT
            std::cout << "DRAM received request from the past Counter: " << m_stat_past_requests << std::endl;
#endif
            SubsecondTime next_free_time = current_interval.end_time + SubsecondTime::NS(1); // +1 ensures availability after this interval ends
            // Overlap found, return the next available cycle after this interval
            return {next_free_time, current_interval};
        }
        preceding_interval = current_interval;
    }

    // If no intervals overlap with pkt_time, bank is free
    m_stat_present_requests++;
#ifdef DEBUG_PRINT
    std::cout << "DRAM received request from the present Counter: " << m_stat_present_requests << std::endl;
#endif
    return {pkt_time, preceding_interval}; // Return pkt_time as the available cycle
}

void DramPerfModelDetailed::cleanupBusyIntervals(IntPtr bank_idx)
{
    m_bank_state_info[bank_idx].m_bank_activity_schedule.pop();
}

void DramPerfModelDetailed::printInterval(std::priority_queue<IntervalNode> intervals)
{
    std::priority_queue<IntervalNode> intervals_copy = intervals;
    while (!intervals_copy.empty())
    {
        IntervalNode current_interval = intervals_copy.top();
        intervals_copy.pop();
#ifdef DEBUG_PRINT
        std::cout << "Start: " << current_interval.start_time.getNS() << " End: " << current_interval.end_time.getNS() << " Page: " << current_interval.open_page << std::endl;
#endif
    }
}

SubsecondTime
DramPerfModelDetailed::getAccessLatency(SubsecondTime request_timestamp, UInt64 packet_size, core_id_t requestor_id, IntPtr mem_address, DramCntlrInterface::access_t access_category, ShmemPerf *perf_stats, bool is_metadata)
{
    UInt64 physical_page_addr = mem_address & ~((UInt64(1) << 12) - 1); // Assuming 4K page
    UInt64 cache_line_addr = mem_address & ~((UInt64(1) << 6) - 1);     // Assuming 64B cache line

    SubsecondTime effective_time = SubsecondTime::Zero();
    if (Sim()->getClockSkewMinimizationServer()->getGlobalTime() > request_timestamp)
    {
        effective_time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();
    }
    else
    {
        effective_time = request_timestamp;
    }

    UInt32 channel_idx, rank_idx, bank_group_idx, bank_idx, column_idx;
    UInt64 page_addr;
    parseDeviceAddress(mem_address, channel_idx, rank_idx, bank_group_idx, bank_idx, column_idx, page_addr);

#ifdef DEBUG_PRINT
    std::cout << "Accessing DRAM data at page " << page_addr << " in bank " << bank_idx << " in bank group " << bank_group_idx << " in rank " << rank_idx << " in channel " << channel_idx << std::endl;
#endif

    SubsecondTime current_latency_time = request_timestamp;
    perf_stats->updateTime(current_latency_time);

    // DDR controller pipeline delay
    current_latency_time += m_mem_ctrl_latency;
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_CNTLR);

    // DDR refresh
    if (m_refresh_cycle_period != SubsecondTime::Zero())
    {
        SubsecondTime refresh_cycle_start = (current_latency_time.getPS() / m_refresh_cycle_period.getPS()) * m_refresh_cycle_period;
        if (current_latency_time - refresh_cycle_start < m_refresh_cycle_duration)
        {
            current_latency_time = refresh_cycle_start + m_refresh_cycle_duration;
            perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_REFRESH);
        }
    }

    // Page hit/miss
    UInt64 combined_bank_idx = (channel_idx * m_rank_count_per_channel * m_bank_count_per_rank) + (rank_idx * m_bank_count_per_rank) + bank_idx;
    LOG_ASSERT_ERROR(combined_bank_idx < m_aggregate_banks, "Bank index out of bounds");
    BankState &bank_state = m_bank_state_info[combined_bank_idx];

    SubsecondTime available_time_start = current_latency_time;

    if (current_latency_time > bank_state.peak_contention_time)
    {
        bank_state.peak_contention_time = current_latency_time;
        bank_state.active_row_address = page_addr;
    }
    else
    {
        auto interval_check_result = fallsWithinInterval(page_addr, current_latency_time, combined_bank_idx);
        available_time_start = interval_check_result.first;
        bank_state.next_available_time = interval_check_result.first;
        bank_state.active_row_address = interval_check_result.second.open_page;
    }

#ifdef DEBUG_PRINT
    printf("[%2d] %s (%12lx, %4lu, %4lu), t_open = %lu, t_now = %lu, bank_state.next_available_time = %lu\n", m_processor_id, bank_state.active_row_address == page_addr && bank_state.next_available_time + m_row_open_duration >= current_latency_time ? "Page Hit: " : "Page Miss:", mem_address, combined_bank_idx, page_addr, current_latency_time.getNS() - bank_state.next_available_time.getNS(), current_latency_time.getNS(), bank_state.next_available_time.getNS());
#endif

    if ((bank_state.active_row_address == page_addr) && (bank_state.next_available_time + m_row_open_duration) >= current_latency_time)
    {
        if (bank_state.next_available_time > current_latency_time)
        {
            current_latency_time = bank_state.next_available_time;
            perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_BANK_PENDING);
#ifdef DEBUG_PRINT
            std::cout << "Row hit but DRAM bank busy, waiting for it to become available at " << current_latency_time.getNS() << std::endl;
#endif
        }
        else
        {
#ifdef DEBUG_PRINT
            std::cout << "Row hit and DRAM bank available at " << current_latency_time.getNS() << std::endl;
#endif
        }
        ++m_stat_row_hits;
    }
    else
    {
        if (bank_state.next_available_time > current_latency_time)
        {
            current_latency_time = bank_state.next_available_time;
#ifdef DEBUG_PRINT
            std::cout << "Row miss, waiting for DRAM bank to become available at " << current_latency_time.getNS() << std::endl;
#endif
        }

        if (bank_state.next_available_time + m_row_open_duration >= current_latency_time)
        {
            if (bank_state.active_row_category == page_type::METADATA && !is_metadata)
            {
                ++m_stat_row_conflict_metadata_to_data;
            }
            if (bank_state.active_row_category == page_type::NOT_METADATA && is_metadata)
            {
                ++m_stat_row_conflict_data_to_metadata;
            }
            if (bank_state.active_row_category == page_type::METADATA && is_metadata)
            {
                ++m_stat_row_conflict_metadata_to_metadata;
            }
            if (bank_state.active_row_category == page_type::NOT_METADATA && !is_metadata)
            {
                ++m_stat_row_conflict_data_to_data;
            }
            current_latency_time += m_row_precharge_latency;
#ifdef DEBUG_PRINT
            std::cout << "Closing DRAM bank at " << current_latency_time.getNS() << std::endl;
#endif
            ++m_stat_row_misses;
        }
        else if (bank_state.next_available_time + m_row_open_duration + m_row_precharge_latency > current_latency_time)
        {
            current_latency_time = bank_state.next_available_time + m_row_open_duration + m_row_precharge_latency;
            ++m_stat_row_closures;
#ifdef DEBUG_PRINT
            std::cout << "Row miss, waiting for DRAM bank to close at " << current_latency_time.getNS() << std::endl;
#endif
        }
        else
        {
            ++m_stat_row_empty_accesses;
        }

        current_latency_time += m_row_activation_latency;
#ifdef DEBUG_PRINT
        std::cout << "Opening DRAM bank at " << current_latency_time.getNS() << std::endl;
#endif
        perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_BANK_CONFLICT);

        if (is_metadata)
        {
            bank_state.active_row_category = page_type::METADATA;
        }
        else
        {
            bank_state.active_row_category = page_type::NOT_METADATA;
        }
        if (is_open_row_buffer_policy)
            bank_state.active_row_address = page_addr;
        else
            bank_state.active_row_address = 0;
    }

    bank_state.owner_core = requestor_id;

    UInt64 combined_rank_idx = (channel_idx * m_rank_count_per_channel) + rank_idx;
    LOG_ASSERT_ERROR(combined_rank_idx < m_aggregate_ranks, "Rank index out of bounds");
    SubsecondTime rank_request_duration = (m_bank_group_count > 1) ? m_short_cmd_delay : m_cmd_to_cmd_delay;
    SubsecondTime rank_queue_delay = m_rank_availability_trackers.size() ? m_rank_availability_trackers[combined_rank_idx]->computeQueueDelay(current_latency_time, rank_request_duration, requestor_id) : SubsecondTime::Zero();

    UInt64 combined_bank_group_idx = (channel_idx * m_rank_count_per_channel * m_bank_group_count) + (rank_idx * m_bank_group_count) + bank_group_idx;
    LOG_ASSERT_ERROR(combined_bank_group_idx < m_aggregate_bank_groups, "Bank-group index out of bounds");
    SubsecondTime group_queue_delay = m_bank_group_availability_trackers.size() ? m_bank_group_availability_trackers[combined_bank_group_idx]->computeQueueDelay(current_latency_time, m_long_cmd_delay, requestor_id) : SubsecondTime::Zero();

    current_latency_time += m_base_access_latency;
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_DEVICE);

#ifdef DEBUG_PRINT
    std::cout << "Finished reading DRAM data at " << current_latency_time.getNS() << std::endl;
#endif

    bank_state.next_available_time = current_latency_time;
    if (bank_state.next_available_time > bank_state.peak_contention_time)
    {
        bank_state.peak_contention_time = bank_state.next_available_time;
        bank_state.peak_contention_page = page_addr;
    }

    IntervalNode new_busy_interval{available_time_start, current_latency_time, page_addr};
    bank_state.m_bank_activity_schedule.push(new_busy_interval);
    printInterval(bank_state.m_bank_activity_schedule);

    if (bank_state.m_bank_activity_schedule.size() > 100)
    {
        cleanupBusyIntervals(combined_bank_idx);
    }

#ifdef DEBUG_PRINT
    std::cout << "Inserted busy interval for DRAM bank " << combined_bank_idx << " from " << available_time_start.getNS() << " to " << current_latency_time.getNS() << "with page: " << page_addr << std::endl;
#endif

    current_latency_time += (rank_queue_delay > group_queue_delay) ? rank_queue_delay : group_queue_delay;
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_DEVICE);

    SubsecondTime bus_transfer_time = m_data_bus_throughput.getRoundedLatency(8 * packet_size); // bytes to bits
    SubsecondTime bus_queue_delay = m_request_queue_models.size() ? m_request_queue_models[channel_idx]->computeQueueDelay(current_latency_time, bus_transfer_time, requestor_id) : SubsecondTime::Zero();
    current_latency_time += bus_queue_delay;

#ifdef DEBUG_PRINT
    std::cout << "There is a queue delay of " << bus_queue_delay.getNS() << " ns" << std::endl;
#endif

    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_QUEUE);
    current_latency_time += bus_transfer_time;
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_BUS);

#ifdef DEBUG_PRINT
    std::cout << "There is a bus delay of " << bus_transfer_time.getNS() << " ns" << std::endl;
#endif

#ifdef DEBUG_PRINT
    std::cout << "Final DRAM Access Latency: " << current_latency_time.getNS() - request_timestamp.getNS() << " Request finished at " << current_latency_time.getNS() << std::endl;
#endif

    return current_latency_time - request_timestamp;
}

SubsecondTime DramPerfModelDetailed::getAccessLatencyUnmodelled(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address)
{
    UInt32 channel_idx, rank_idx, bank_group_idx, bank_idx, column_idx;
    UInt64 page_addr;
    parseDeviceAddress(address, channel_idx, rank_idx, bank_group_idx, bank_idx, column_idx, page_addr);

    SubsecondTime bus_transfer_time = m_data_bus_throughput.getRoundedLatency(8 * pkt_size); // bytes to bits
    SubsecondTime bus_queue_delay = m_request_queue_models.size() ? m_request_queue_models[channel_idx]->computeQueueDelay(pkt_time, bus_transfer_time, requester) : SubsecondTime::Zero();
    return bus_transfer_time + bus_queue_delay;
}