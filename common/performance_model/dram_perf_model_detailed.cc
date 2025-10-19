#include "dram_perf_model_detailed.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "stats.h"
#include "shmem_perf.h"
#include "subsecond_time.h"
#include "utils.h"

// #define DEBUG_PRINT

/*
 * DramPerfModelDetailed
 * ---------------------
 * A cycle/interval–level DRAM performance model with:
 *  - Address decomposition into DDR channel/rank/bank-group/bank/column/page.
 *  - Open/closed-row page policies (configurable).
 *  - Rank & bank-group command spacing using small per-resource queue models.
 *  - Per-bank busy-interval tracking to model overlap/conflict and row-buffer hits.
 *  - Periodic refresh windows.
 *  - A shared channel request queue model to represent data bus contention.
 *
 * Notable modeling choices:
 *  - We treat page == DRAM row (aka "row address" here).
 *  - We maintain for each bank:
 *      - next_available_time (last device event completion for that bank)
 *      - peak_contention_time/page (high-watermark time/page observed)
 *      - a priority_queue of busy intervals (newest at top; see IntervalNode comparator)
 *      - active_row_address & active_row_category (METADATA/NOT_METADATA) to count conflicts
 *  - We record detailed stats: row hits/misses/empty/closes and metadata/data conflict types,
 *    and whether requests arrive in the past/present relative to known intervals.
 *
 * Timing conventions:
 *  - All time is SubsecondTime (ns/ps domain), sourced from the simulator's clock.
 *  - Inter-command delays and device latencies are configured and added at appropriate points.
 *  - QueueModel::computeQueueDelay() returns delays to respect throughput/spacing constraints.
 *
 * Address mapping conventions:
 *  - m_use_open_row_policy governs how bits are arranged (open-page vs closed-page style).
 *  - Low-level bit slicing depends on configured offsets and sizes; we assert boundaries.
 */

DramPerfModelDetailed::DramPerfModelDetailed(
    core_id_t core_id,
    UInt32 cache_block_size,
    AddressHomeLookup *address_home_lookup
)
    // Base-class: track core + line size
    : DramPerfModel(core_id, cache_block_size)
    // Per-instance identity/config references
    , m_processor_id(core_id)
    , m_addr_home_locator(address_home_lookup)

    // --- Topology parameters (static config) ---
    , m_bank_count_per_rank(         Sim()->getCfg()->getInt("perf_model/dram/ddr/num_banks"))
    , m_bank_count_per_rank_log2(     floorLog2(m_bank_count_per_rank))
    , m_bank_group_count(             Sim()->getCfg()->getInt("perf_model/dram/ddr/num_bank_groups"))
    , m_rank_count_per_channel(       Sim()->getCfg()->getInt("perf_model/dram/ddr/num_ranks"))
    , m_rank_bit_offset(              Sim()->getCfg()->getInt("perf_model/dram/ddr/rank_offset"))
    , m_channel_count(                Sim()->getCfg()->getInt("perf_model/dram/ddr/num_channels"))
    , m_channel_bit_offset(           Sim()->getCfg()->getInt("perf_model/dram/ddr/channel_offset"))
    , m_home_locator_bit(             Sim()->getCfg()->getInt("perf_model/dram_directory/home_lookup_param"))

    // Derived counts for convenience
    , m_aggregate_ranks(              m_rank_count_per_channel * m_channel_count)
    , m_banks_per_channel_count(      m_bank_count_per_rank * m_rank_count_per_channel)
    , m_banks_per_group_count(        m_bank_count_per_rank / m_bank_group_count)
    , m_aggregate_banks(              m_banks_per_channel_count * m_channel_count)
    , m_aggregate_bank_groups(        m_bank_group_count * m_rank_count_per_channel * m_channel_count)

    // --- Data bus + frequency ---
    , m_bus_bit_width(                Sim()->getCfg()->getInt("perf_model/dram/ddr/data_bus_width"))      // bits
    , m_dram_frequency_mhz(           Sim()->getCfg()->getInt("perf_model/dram/ddr/dram_speed"))          // MT/s
    // Row-buffer/page geometry
    , m_row_buffer_size_bytes(        Sim()->getCfg()->getInt("perf_model/dram/ddr/dram_page_size"))
    , m_row_buffer_size_bytes_log2(    floorLog2(m_row_buffer_size_bytes))
    , m_use_open_row_policy(          Sim()->getCfg()->getBool("perf_model/dram/ddr/open_page_mapping"))

    // Column/bank mapping bit positions
    , m_col_bit_offset(               Sim()->getCfg()->getInt("perf_model/dram/ddr/column_offset"))
    , m_col_high_bit_offset(          m_row_buffer_size_bytes_log2 - m_col_bit_offset + m_bank_count_per_rank_log2)
    , m_bank_bit_offset(              m_row_buffer_size_bytes_log2 - m_col_bit_offset)

    // Optional address randomization knobs
    , m_enable_address_randomization( Sim()->getCfg()->getBool("perf_model/dram/ddr/randomize_address"))
    , m_randomization_bit_offset(     Sim()->getCfg()->getInt("perf_model/dram/ddr/randomize_offset"))
    , m_col_bits_position(            Sim()->getCfg()->getInt("perf_model/dram/ddr/column_bits_shift"))

    // Effective data throughput: (MT/s * bits/transfer) / 1000 => bits/ns
    // Note: "MT/s" here is effectively mega-transfers per second; dividing by 1000 converts to transfers/ns.
    , m_data_bus_throughput(          m_dram_frequency_mhz * m_bus_bit_width / 1000)

    // --- DRAM timing parameters (all in ns) ---
    , m_row_open_duration(            SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_keep_open")))
    , m_row_activation_latency(       SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_open_delay")))
    , m_row_precharge_latency(        SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_close_delay")))
    , m_base_access_latency(          SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/access_cost")))
    , m_cmd_to_cmd_delay(             SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay")))
    , m_short_cmd_delay(              SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay_short")))
    , m_long_cmd_delay(               SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay_long")))
    , m_mem_ctrl_latency(             SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/controller_delay")))
    , m_refresh_cycle_period(         SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/refresh_interval")))
    , m_refresh_cycle_duration(       SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/refresh_length")))
    , m_local_dram_capacity_threshold( Sim()->getCfg()->getInt("perf_model/dram/localdram_size"))          // Not used here; threshold to migrate data to local DRAM

    // --- State containers / counters ---
    , m_bank_state_info(m_aggregate_banks)            // vector<BankState>, sized to total banks
    , m_stat_row_hits(0)
    , m_stat_row_empty_accesses(0)
    , m_stat_row_closures(0)
    , m_stat_row_misses(0)
    , m_stat_past_requests(0)
    , m_stat_unknown_past_requests(0)
    , m_stat_present_requests(0)
    , m_cumulative_queue_latency(SubsecondTime::Zero())
    , m_cumulative_access_latency(SubsecondTime::Zero())

    // Behavior policy flags
    , is_fixed_latency_policy(             Sim()->getCfg()->getBool("perf_model/dram/ddr/constant_time_policy"))
    , is_selective_fixed_latency_policy(   Sim()->getCfg()->getBool("perf_model/dram/ddr/selective_constant_time_policy"))
    , is_open_row_buffer_policy(           Sim()->getCfg()->getBool("perf_model/dram/ddr/open_row_policy"))

    // Required bit widths (for sanity in parseDeviceAddress)
    , m_needed_bank_bits(    Sim()->getCfg()->getInt("perf_model/dram/ddr/required_bank_bits"))
    , m_needed_rank_bits(    Sim()->getCfg()->getInt("perf_model/dram/ddr/required_rank_bits"))
    , m_needed_channel_bits( Sim()->getCfg()->getInt("perf_model/dram/ddr/required_channel_bits"))
    , m_dram_row_size_bits(  Sim()->getCfg()->getInt("perf_model/dram/ddr/dram_page_size_bits"))
{
    // Optional per-channel bus queue (models shared data-bus contention)
    String model_name("dram");
    if (Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled"))
    {
        for (UInt32 chan_idx = 0; chan_idx < m_channel_count; ++chan_idx)
        {
            // QueueModel for channel data bus:
            // Name: "dram-queue-<chan>"
            // The service time is rounded latency for 8 bytes (converted by getRoundedLatency below)
            m_request_queue_models.push_back(QueueModel::create(
                model_name + "-queue-" + itostr(chan_idx),
                core_id,
                Sim()->getCfg()->getString("perf_model/dram/queue_model/type"),
                m_data_bus_throughput.getRoundedLatency(8)  // baseline quantum (bytes -> bits handled inside)
            ));
        }
    }

    // Expose cumulative access latency as a top-level metric
    registerStatsMetric("dram", core_id, "total-access-latency", &m_cumulative_access_latency);

    // Per-(channel,rank) command spacing (prevents back-to-back rank commands)
    for (UInt32 rank_idx = 0; rank_idx < m_aggregate_ranks; ++rank_idx)
    {
        m_rank_availability_trackers.push_back(QueueModel::create(
            model_name + "-rank-" + itostr(rank_idx),
            core_id,
            "history_list",                                       // lightweight spacing via historical endpoints
            (m_bank_group_count > 1) ? m_short_cmd_delay : m_cmd_to_cmd_delay
        ));
    }

    // Per-(channel,rank,bank-group) long spacing (e.g., tCCD_L–like)
    for (UInt32 group_idx = 0; group_idx < m_aggregate_bank_groups; ++group_idx)
    {
        m_bank_group_availability_trackers.push_back(QueueModel::create(
            model_name + "-bank-group-" + itostr(group_idx),
            core_id,
            "history_list",
            m_long_cmd_delay
        ));
    }

    // Initialize per-bank state
    for (UInt32 bank_idx = 0; bank_idx < m_aggregate_banks; ++bank_idx)
    {
        m_bank_state_info[bank_idx].owner_core           = -1;
        m_bank_state_info[bank_idx].active_row_address   = -1;                 // -1 means "no open row"
        m_bank_state_info[bank_idx].next_available_time  = SubsecondTime::Zero();
        m_bank_state_info[bank_idx].peak_contention_time = SubsecondTime::Zero();
        m_bank_state_info[bank_idx].peak_contention_page = -1;
        m_bank_state_info[bank_idx].active_row_category  = NOT_METADATA;
        // m_bank_activity_schedule starts empty
    }

    // Configuration sanity checks
    LOG_ASSERT_ERROR(cache_block_size == 64, "Hardcoded for 64-byte cache lines");
    LOG_ASSERT_ERROR(m_col_bit_offset <= m_row_buffer_size_bytes_log2, "Column offset exceeds bounds!");
    if (m_enable_address_randomization)
        LOG_ASSERT_ERROR(m_bank_group_count == 4 || m_bank_group_count == 8, "Number of bank groups incorrect for address randomization!");

    // Row-buffer behavior stats
    registerStatsMetric("ddr",  core_id, "page-hits",                             &m_stat_row_hits);
    registerStatsMetric("ddr",  core_id, "page-empty",                            &m_stat_row_empty_accesses);
    registerStatsMetric("ddr",  core_id, "page-closing",                          &m_stat_row_closures);
    registerStatsMetric("ddr",  core_id, "page-miss",                             &m_stat_row_misses);

    // Temporal classification of requests vis-à-vis known intervals
    registerStatsMetric("dram", core_id, "received-request-from-the-past",         &m_stat_past_requests);
    registerStatsMetric("dram", core_id, "received-request-from-the-unknown-past", &m_stat_unknown_past_requests);
    registerStatsMetric("dram", core_id, "received-request-from-present",          &m_stat_present_requests);
}

DramPerfModelDetailed::~DramPerfModelDetailed()
{
    // Free all queue models (if allocated)
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

/*
 * parseAddressBits()
 * Utility: extract 'size' values from 'address' at bit 'offset' (mod size), placing
 * the decoded piece in 'data'. Optionally decode from 'base_address' instead.
 * Returns the original address with that bit-field "removed" (compacted): i.e., it
 * shifts the higher bits down past the removed field so downstream parsing can be simpler.
 */
UInt64
DramPerfModelDetailed::parseAddressBits(UInt64 address, UInt32 &data, UInt32 offset, UInt32 size, UInt64 base_address /*= 0*/)
{
    UInt32 size_log2 = floorLog2(size);
    if (base_address != 0)
    {
        data = (base_address >> offset) % size;
    }
    else
    {
        data = (address >> offset) % size;
    }
    // Reconstruct: [higher bits << offset] | [lower bits below the sliced field]
    return ((address >> (offset + size_log2)) << offset) | (address & ((1 << offset) - 1));
}

/*
 * parseDeviceAddress()
 * Map a linearized physical address to DDR topology coordinates:
 *   out_channel, out_rank, out_bank_group, out_bank, out_column, out_page
 *
 * Steps:
 *  - Convert to "flat" (interleaving removed) address via AddressHomeLookup.
 *  - Drop the 64B line offset (>> 6) to work in line granularity.
 *  - Depending on open/closed page mapping and configured bit offsets, slice column/bank/group/rank/channel.
 *  - out_page retains the remaining high-order bits (row address).
 *
 * Notes:
 *  - In open-page mapping (typical row-buffer-friendly scheme), the column is formed from low bits (possibly split),
 *    then bank/bank-group, then higher bits form the page.
 *  - In closed-page, we fold differently: column comes from a position, remaining form page (row), with channel/rank/bank
 *    already extracted from the lower part.
 */
void DramPerfModelDetailed::parseDeviceAddress(
    IntPtr address,
    UInt32 &out_channel,
    UInt32 &out_rank,
    UInt32 &out_bank_group,
    UInt32 &out_bank,
    UInt32 &out_column,
    UInt64 &out_page
){
    // Remove directory/home interleaving: get an address purely in physical space
    UInt64 flat_address = m_addr_home_locator->getLinearAddress(address);

    // Discard 64B line offset; we address at cache-line granularity here
    UInt64 relevant_address_bits = flat_address >> 6;

#ifdef DEBUG_PRINT
    std::cout << "Address bits: " << std::bitset<64>(relevant_address_bits) << std::endl;
#endif

    if (m_use_open_row_policy)
    {
        // --- Open-page mapping ---
        // Order (low->high) resembles: ColLo | Bank | ColHi | Page
        if (m_col_bit_offset)
        {
            // If column is split, m_col_bit_offset = #ColHi bits.
            // Column = (ColHi << bank_bit_width) | ColLo (mod page size)
            out_column = (
                ((relevant_address_bits >> m_col_high_bit_offset) << m_bank_bit_offset) |
                (relevant_address_bits & ((1 << m_bank_bit_offset) - 1))
            ) % m_row_buffer_size_bytes;

            // Advance past bank bits (used in Column above)
            relevant_address_bits = relevant_address_bits >> m_bank_bit_offset;

            // Bank-group and bank share low bits; "% count" extracts within range.
            out_bank_group = relevant_address_bits % m_bank_group_count;
            out_bank       = relevant_address_bits % m_bank_count_per_rank;

            // Skip high (ColHi) + bank bits to reach Page
            relevant_address_bits = relevant_address_bits >> (m_bank_count_per_rank_log2 + m_col_bit_offset);
        }
        else
        {
            // If not split, explicitly peel off channel, column, rank, group, bank in-order per configured widths.

            out_channel = relevant_address_bits % m_channel_count;
            relevant_address_bits = relevant_address_bits >> m_needed_channel_bits;
#ifdef DEBUG_PRINT
            std::cout << "Channel: " << out_channel << std::endl;
#endif

            out_column = relevant_address_bits % m_row_buffer_size_bytes;  // Size == page/row buffer size
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

        // Remaining high bits are the row/page
        out_page = relevant_address_bits;
#ifdef DEBUG_PRINT
        std::cout << "Page: " << out_page << std::endl;
#endif
    }
    else
    {
        // --- Closed-page mapping ---
        // Here we compute bank(group)/bank first from low bits, then compute column from m_col_bits_position,
        // and page (row) from the remaining upper bits.
        out_bank_group = relevant_address_bits % m_bank_group_count;
        out_bank       = relevant_address_bits % m_bank_count_per_rank;
        relevant_address_bits /= m_bank_count_per_rank;

        // Column is at a fixed bit position (shift), mod row-buffer size
        out_column = (relevant_address_bits >> m_col_bits_position) % m_row_buffer_size_bytes;

        // out_page retains high bits (above column) plus the lower bits below the column window
        out_page = (
            ((relevant_address_bits >> m_col_bits_position) / m_row_buffer_size_bytes) << m_col_bits_position
        ) | (relevant_address_bits & ((1 << m_col_bits_position) - 1));
    }
}

/*
 * fallsWithinInterval(page, pkt_time, bank_idx)
 * ---------------------------------------------
 * Given a desired packet time, consult the per-bank busy-interval priority queue to find:
 *  - If pkt_time falls within an existing busy interval -> return the first free time just after it.
 *  - If pkt_time occurs before the earliest known interval (unknown prior activity) -> classify as unknown past.
 *  - Else if pkt_time is before an interval but after some prior interval -> classify as "past".
 *  - If no conflicts -> "present", and pkt_time is usable.
 *
 * Returns:
 *  - pair<available_time, IntervalNode-of-overlap-or-preceding>:
 *      - available_time: earliest time the bank is free for this access
 *      - the IntervalNode gives context (e.g., overlapping open_page)
 *
 * Implementation note:
 *  - We copy the queue (heap) to avoid mutating original schedule.
 *  - The queue top is the most recent interval; we iterate backwards in time.
 */
std::pair<SubsecondTime, DramPerfModelDetailed::IntervalNode>
DramPerfModelDetailed::fallsWithinInterval(UInt64 page, SubsecondTime pkt_time, IntPtr bank_idx)
{
    if (m_bank_state_info[bank_idx].m_bank_activity_schedule.empty())
    {
        // No prior intervals: bank is immediately available at pkt_time
        return {pkt_time, IntervalNode(SubsecondTime::Zero(), SubsecondTime::Zero(), -1)};
    }

    std::priority_queue<IntervalNode> intervals_copy = m_bank_state_info[bank_idx].m_bank_activity_schedule;

    // "preceding_interval" holds the most recent interval that ends before pkt_time
    IntervalNode preceding_interval;
    preceding_interval.start_time = SubsecondTime::Zero();
    preceding_interval.end_time   = SubsecondTime::Zero();
    preceding_interval.open_page  = -1;

    while (!intervals_copy.empty())
    {
        IntervalNode current_interval = intervals_copy.top();
        intervals_copy.pop();

        // If the next (earlier) interval starts after our pkt_time, we've passed the moment we're interested in.
        if (current_interval.start_time > pkt_time)
        {
            // If there was no prior interval before pkt_time, this is "unknown past":
            // pkt_time precedes the earliest known busy period.
            if (preceding_interval.open_page == -1)
            {
                m_stat_unknown_past_requests++;
#ifdef DEBUG_PRINT
                std::cout << "DRAM received request from the unknown past Counter: " << m_stat_unknown_past_requests << std::endl;
                std::cout << "pkt_time: " << pkt_time.getNS()
                          << " current_interval.start_time: " << current_interval.start_time.getNS()
                          << " current_interval.end_time: "   << current_interval.end_time.getNS()
                          << "max_time: " << m_bank_state_info[bank_idx].peak_contention_time.getNS() << std::endl;
#endif
            }
            else
            {
                // Else we did have a prior interval; classify as "past" relative to known schedule.
                m_stat_past_requests++;
#ifdef DEBUG_PRINT
                std::cout << "DRAM received request from the past Counter: " << m_stat_past_requests << std::endl;
#endif
            }
            // Either way, pkt_time is acceptable since we are before this future interval.
            return {pkt_time, preceding_interval};
        }

        // Overlap: pkt_time falls inside an existing busy interval -> move to first time after it
        if (current_interval.start_time <= pkt_time && current_interval.end_time >= pkt_time)
        {
            m_stat_past_requests++;
#ifdef DEBUG_PRINT
            std::cout << "DRAM received request from the past Counter: " << m_stat_past_requests << std::endl;
#endif
            SubsecondTime next_free_time = current_interval.end_time + SubsecondTime::NS(1); // +1 ps/ns to ensure strictly after
            return {next_free_time, current_interval};
        }

        // Track most recent interval we've seen so far that ends before pkt_time
        preceding_interval = current_interval;
    }

    // No intervals overlap pkt_time, and we've exhausted the schedule: treat as "present" (no conflicts)
    m_stat_present_requests++;
#ifdef DEBUG_PRINT
    std::cout << "DRAM received request from the present Counter: " << m_stat_present_requests << std::endl;
#endif
    return {pkt_time, preceding_interval};
}

/*
 * cleanupBusyIntervals(bank_idx)
 * Housekeeping: If the busy-interval queue grows too large, pop one
 * (oldest/redundant per comparator) to cap memory usage. Policy: called when size > 100.
 */
void DramPerfModelDetailed::cleanupBusyIntervals(IntPtr bank_idx)
{
    m_bank_state_info[bank_idx].m_bank_activity_schedule.pop();
}

/*
 * printInterval(intervals)
 * Debug helper: dumps the queue contents (most-recent first) when DEBUG_PRINT is defined.
 */
void DramPerfModelDetailed::printInterval(std::priority_queue<IntervalNode> intervals)
{
    std::priority_queue<IntervalNode> intervals_copy = intervals;
    while (!intervals_copy.empty())
    {
        IntervalNode current_interval = intervals_copy.top();
        intervals_copy.pop();
#ifdef DEBUG_PRINT
        std::cout << "Start: " << current_interval.start_time.getNS()
                  << " End: "   << current_interval.end_time.getNS()
                  << " Page: "  << current_interval.open_page
                  << std::endl;
#endif
    }
}

/*
 * getAccessLatency()
 * ------------------
 * Full DRAM access timeline for a request to 'mem_address':
 *   1) Start at max(global_time, request_timestamp).
 *   2) Add controller pipeline delay.
 *   3) If within refresh window, wait to the end of refresh.
 *   4) Resolve bank conflicts/row-buffer hits vs misses using per-bank state & intervals:
 *        - If same row open within keep-open duration and bank is free => row hit.
 *        - Else precharge/close if needed, activate new row (open delay), mark stats.
 *   5) Enforce rank/bank-group inter-command spacing via queue models (rank/group).
 *   6) Perform base device access latency.
 *   7) Update bank next_available_time, push a new busy interval for bookkeeping.
 *   8) Compute shared channel bus queue delay, then add bus transfer time (size-dependent).
 *   9) Return total latency (end - request_timestamp).
 */
SubsecondTime
DramPerfModelDetailed::getAccessLatency(SubsecondTime request_timestamp,
                                        UInt64 packet_size,
                                        core_id_t requestor_id,
                                        IntPtr mem_address,
                                        DramCntlrInterface::access_t access_category,
                                        ShmemPerf *perf_stats,
                                        bool is_metadata)
{
    // Convenient masks for page/line alignment (assumes 4KB pages and 64B lines)
    UInt64 physical_page_addr = mem_address & ~((UInt64(1) << 12) - 1); // 4KB page align
    UInt64 cache_line_addr    = mem_address & ~((UInt64(1) << 6)  - 1); // 64B line align
    (void)physical_page_addr; (void)cache_line_addr; // currently unused, but kept for clarity/extension

    // Requests cannot start earlier than global time (clock skew mitigation)
    SubsecondTime effective_time = SubsecondTime::Zero();
    if (Sim()->getClockSkewMinimizationServer()->getGlobalTime() > request_timestamp)
        effective_time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();
    else
        effective_time = request_timestamp;

    // Decode address to channel/rank/bank etc.
    UInt32 channel_idx, rank_idx, bank_group_idx, bank_idx, column_idx;
    UInt64 page_addr;
    parseDeviceAddress(mem_address, channel_idx, rank_idx, bank_group_idx, bank_idx, column_idx, page_addr);

#ifdef DEBUG_PRINT
    std::cout << "Accessing DRAM data at page " << page_addr
              << " in bank " << bank_idx
              << " in bank group " << bank_group_idx
              << " in rank " << rank_idx
              << " in channel " << channel_idx << std::endl;
#endif

    // Begin timeline at the caller-specified request timestamp (not the clamped effective_time),
    // but we use effective_time for device availability checks below.
    SubsecondTime current_latency_time = request_timestamp;
    perf_stats->updateTime(current_latency_time);

    // (1) Memory controller pipeline latency
    current_latency_time += m_mem_ctrl_latency;
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_CNTLR);

    // (2) Periodic refresh handling: if we are inside the refresh duration, wait it out
    if (m_refresh_cycle_period != SubsecondTime::Zero())
    {
        SubsecondTime refresh_cycle_start = (current_latency_time.getPS() / m_refresh_cycle_period.getPS()) * m_refresh_cycle_period;
        if (current_latency_time - refresh_cycle_start < m_refresh_cycle_duration)
        {
            current_latency_time = refresh_cycle_start + m_refresh_cycle_duration;
            perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_REFRESH);
        }
    }

    // Compute linear bank index across all channels/ranks
    UInt64 combined_bank_idx =
        (channel_idx * m_rank_count_per_channel * m_bank_count_per_rank) +
        (rank_idx    * m_bank_count_per_rank) +
        bank_idx;

    LOG_ASSERT_ERROR(combined_bank_idx < m_aggregate_banks, "Bank index out of bounds");
    BankState &bank_state = m_bank_state_info[combined_bank_idx];

    // The time when we can begin issuing to this bank
    SubsecondTime available_time_start = current_latency_time;

    // If this request lands beyond the last contention we’ve seen, update high-watermark
    if (current_latency_time > bank_state.peak_contention_time)
    {
        bank_state.peak_contention_time = current_latency_time;
        bank_state.active_row_address   = page_addr; // speculative: assume this is the active row from now
    }
    else
    {
        // Otherwise, we are somewhere at/behind known peak; consult intervals for overlap/conflict.
        auto interval_check_result = fallsWithinInterval(page_addr, current_latency_time, combined_bank_idx);
        available_time_start             = interval_check_result.first;
        bank_state.next_available_time   = interval_check_result.first;
        bank_state.active_row_address    = interval_check_result.second.open_page; // context row
    }

#ifdef DEBUG_PRINT
    printf("[%2d] %s (%12lx, %4lu, %4lu), t_open = %lu, t_now = %lu, bank_state.next_available_time = %lu\n",
           m_processor_id,
           bank_state.active_row_address == page_addr && bank_state.next_available_time + m_row_open_duration >= current_latency_time ? "Page Hit: " : "Page Miss:",
           mem_address, combined_bank_idx, page_addr,
           current_latency_time.getNS() - bank_state.next_available_time.getNS(),
           current_latency_time.getNS(),
           bank_state.next_available_time.getNS());
#endif

    // (3) Row-buffer decision: hit if same row open and still within keep-open window
    if ((bank_state.active_row_address == page_addr) &&
        (bank_state.next_available_time + m_row_open_duration) >= current_latency_time)
    {
        // Row hit, but we may need to wait if bank is still busy with a previous access
        if (bank_state.next_available_time > current_latency_time)
        {
            current_latency_time = bank_state.next_available_time;
            perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_BANK_PENDING);
#ifdef DEBUG_PRINT
            std::cout << "Row hit but DRAM bank busy, waiting until "
                      << current_latency_time.getNS() << std::endl;
#endif
        }
        ++m_stat_row_hits;
    }
    else
    {
        // Row miss/conflict path:
        // If bank still busy, wait; then possibly precharge/close, then activate new row.
        if (bank_state.next_available_time > current_latency_time)
        {
            current_latency_time = bank_state.next_available_time;
#ifdef DEBUG_PRINT
            std::cout << "Row miss, waiting for DRAM bank to become available at "
                      << current_latency_time.getNS() << std::endl;
#endif
        }

        // If prior row still considered open within keep-open window, count conflicts and precharge
        if (bank_state.next_available_time + m_row_open_duration >= current_latency_time)
        {

            current_latency_time += m_row_precharge_latency; // precharge/close
#ifdef DEBUG_PRINT
            std::cout << "Closing DRAM bank at " << current_latency_time.getNS() << std::endl;
#endif
            ++m_stat_row_misses;
        }
        else if (bank_state.next_available_time + m_row_open_duration + m_row_precharge_latency > current_latency_time)
        {
            // We arrived while a close is in-flight; wait until it completes
            current_latency_time = bank_state.next_available_time + m_row_open_duration + m_row_precharge_latency;
            ++m_stat_row_closures;
#ifdef DEBUG_PRINT
            std::cout << "Row miss, waiting for DRAM bank to close at "
                      << current_latency_time.getNS() << std::endl;
#endif
        }
        else
        {
            // The row was already closed long ago; empty-row access
            ++m_stat_row_empty_accesses;
        }

        // Activate the desired row (tRCD-like)
        current_latency_time += m_row_activation_latency;
#ifdef DEBUG_PRINT
        std::cout << "Opening DRAM bank at " << current_latency_time.getNS() << std::endl;
#endif
        perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_BANK_CONFLICT);

        // Mark new active row category for future conflict stats
        bank_state.active_row_category = is_metadata ? page_type::METADATA : page_type::NOT_METADATA;
        // If open-row-buffer policy: remember the page; otherwise we don't keep it "open" logically
        bank_state.active_row_address  = is_open_row_buffer_policy ? page_addr : 0;
    }

    // Mark ownership (last issuing core)
    bank_state.owner_core = requestor_id;

    // (4) Enforce rank & bank-group command spacing via separate queue models
    UInt64 combined_rank_idx = (channel_idx * m_rank_count_per_channel) + rank_idx;
    LOG_ASSERT_ERROR(combined_rank_idx < m_aggregate_ranks, "Rank index out of bounds");
    SubsecondTime rank_request_duration = (m_bank_group_count > 1) ? m_short_cmd_delay : m_cmd_to_cmd_delay;
    SubsecondTime rank_queue_delay = m_rank_availability_trackers.size()
        ? m_rank_availability_trackers[combined_rank_idx]->computeQueueDelay(current_latency_time, rank_request_duration, requestor_id)
        : SubsecondTime::Zero();

    UInt64 combined_bank_group_idx = (channel_idx * m_rank_count_per_channel * m_bank_group_count)
                                   + (rank_idx * m_bank_group_count)
                                   + bank_group_idx;
    LOG_ASSERT_ERROR(combined_bank_group_idx < m_aggregate_bank_groups, "Bank-group index out of bounds");
    SubsecondTime group_queue_delay = m_bank_group_availability_trackers.size()
        ? m_bank_group_availability_trackers[combined_bank_group_idx]->computeQueueDelay(current_latency_time, m_long_cmd_delay, requestor_id)
        : SubsecondTime::Zero();

    // (5) Device access latency proper (CAS/burst core), excluding bus transfer
    current_latency_time += m_base_access_latency;
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_DEVICE);

#ifdef DEBUG_PRINT
    std::cout << "Finished reading DRAM data at " << current_latency_time.getNS() << std::endl;
#endif

    // Update per-bank "last activity" and peak contention tracking
    bank_state.next_available_time = current_latency_time;
    if (bank_state.next_available_time > bank_state.peak_contention_time)
    {
        bank_state.peak_contention_time = bank_state.next_available_time;
        bank_state.peak_contention_page = page_addr;
    }

    // Record the busy interval [available_time_start, current_latency_time] for this access
    IntervalNode new_busy_interval{available_time_start, current_latency_time, page_addr};
    bank_state.m_bank_activity_schedule.push(new_busy_interval);
    printInterval(bank_state.m_bank_activity_schedule);

    // Keep the queue from growing without bound (policy threshold = 100)
    if (bank_state.m_bank_activity_schedule.size() > 100)
    {
        cleanupBusyIntervals(combined_bank_idx);
    }

#ifdef DEBUG_PRINT
    std::cout << "Inserted busy interval for DRAM bank " << combined_bank_idx
              << " from " << available_time_start.getNS()
              << " to "   << current_latency_time.getNS()
              << " with page: " << page_addr << std::endl;
#endif

    // Respect whichever spacing constraint is stricter (rank vs bank-group)
    current_latency_time += (rank_queue_delay > group_queue_delay) ? rank_queue_delay : group_queue_delay;
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_DEVICE);

    // (6) Shared channel data bus:
    // Transfer time scales with packet_size (bytes -> bits inside throughput helper)
    SubsecondTime bus_transfer_time = m_data_bus_throughput.getRoundedLatency(8 * packet_size); // bytes->bits
    SubsecondTime bus_queue_delay   = m_request_queue_models.size()
        ? m_request_queue_models[channel_idx]->computeQueueDelay(current_latency_time, bus_transfer_time, requestor_id)
        : SubsecondTime::Zero();

    current_latency_time += bus_queue_delay;
#ifdef DEBUG_PRINT
    std::cout << "There is a queue delay of " << bus_queue_delay.getNS() << " ns" << std::endl;
#endif
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_QUEUE);

    current_latency_time += bus_transfer_time;
    perf_stats->updateTime(current_latency_time, ShmemPerf::DRAM_BUS);
#ifdef DEBUG_PRINT
    std::cout << "There is a bus delay of " << bus_transfer_time.getNS() << " ns" << std::endl;
    std::cout << "Final DRAM Access Latency: "
              << current_latency_time.getNS() - request_timestamp.getNS()
              << " Request finished at " << current_latency_time.getNS() << std::endl;
#endif

    // Return end-to-end latency as seen by caller
    return current_latency_time - request_timestamp;
}

/*
 * getAccessLatencyUnmodelled()
 * ----------------------------
 * Lightweight path when detailed device timing is disabled/undesired:
 *  - Only computes channel bus transfer time + queueing on the data bus.
 *  - Still decodes channel to choose the correct per-channel queue.
 */
SubsecondTime
DramPerfModelDetailed::getAccessLatencyUnmodelled(SubsecondTime pkt_time,
                                                  UInt64 pkt_size,
                                                  core_id_t requester,
                                                  IntPtr address)
{
    UInt32 channel_idx, rank_idx, bank_group_idx, bank_idx, column_idx;
    UInt64 page_addr;
    parseDeviceAddress(address, channel_idx, rank_idx, bank_group_idx, bank_idx, column_idx, page_addr);

    SubsecondTime bus_transfer_time = m_data_bus_throughput.getRoundedLatency(8 * pkt_size); // bytes to bits
    SubsecondTime bus_queue_delay   = m_request_queue_models.size()
        ? m_request_queue_models[channel_idx]->computeQueueDelay(pkt_time, bus_transfer_time, requester)
        : SubsecondTime::Zero();

    return bus_transfer_time + bus_queue_delay;
}
