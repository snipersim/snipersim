/**
 * @file dram_perf_model_detailed.cc
 * @brief Detailed DDR DRAM Performance Model
 * 
 * This model simulates DDR memory access latency with support for:
 * - Open/closed page policies with row buffer management
 * - Bank, rank, channel, and bank group modeling
 * - Refresh timing
 * - Command bus and data bus contention
 * - Out-of-order request handling with interval tracking
 * - Metadata vs data conflict tracking for security research
 * 
 * Address Mapping (open-page, no column offset):
 *   | Row/Page | Bank | Rank | Column | Channel | (cache line offset removed) |
 * 
 * The model tracks bank busy intervals to handle out-of-order requests
 * that may arrive with timestamps in the "past" relative to the bank state.
 */

#include "dram_perf_model_detailed.h"
#include "dram_address_mapping_factory.h"
#include "metadata_info.h"
#include "debug_config.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "stats.h"
#include "shmem_perf.h"
#include "subsecond_time.h"
#include "utils.h"

#include <set>
#include <sstream>

// Enable detailed per-access DRAM latency logging (WARNING: very large output files!)
// Only enable for debugging specific DRAM access patterns
// Requires DEBUG_DRAM_CSV to be set in debug_config.h
// #define ENABLE_DRAM_LATENCY_CSV

//=============================================================================
// Constructor
//=============================================================================

DramPerfModelDetailed::DramPerfModelDetailed(core_id_t core_id, UInt32 cache_block_size, AddressHomeLookup* address_home_lookup, const String& suffix)
   : DramPerfModel(core_id, cache_block_size, suffix)
   , m_core_id(core_id)
   , m_address_home_lookup(address_home_lookup)
   , m_address_mapping(DramAddressMappingFactory::create(address_home_lookup))
   // DRAM organization parameters
   , m_num_banks           (Sim()->getCfg()->getInt("perf_model/dram/ddr/num_banks"))
   , m_num_banks_log2      (floorLog2(m_num_banks))
   , m_num_bank_groups     (Sim()->getCfg()->getInt("perf_model/dram/ddr/num_bank_groups"))
   , m_num_ranks           (Sim()->getCfg()->getInt("perf_model/dram/ddr/num_ranks"))
   , m_num_channels        (Sim()->getCfg()->getInt("perf_model/dram/ddr/num_channels"))
   // Derived organization values
   , m_total_ranks         (m_num_ranks * m_num_channels)
   , m_banks_per_channel   (m_num_banks * m_num_ranks)
   , m_banks_per_bank_group(m_num_banks / m_num_bank_groups)
   , m_total_banks         (m_banks_per_channel * m_num_channels)
   , m_total_bank_groups   (m_num_bank_groups * m_num_ranks * m_num_channels)
   // Bus and speed parameters
   , m_data_bus_width      (Sim()->getCfg()->getInt("perf_model/dram/ddr/data_bus_width"))
   , m_dram_speed          (Sim()->getCfg()->getInt("perf_model/dram/ddr/dram_speed"))
   , m_dram_page_size      (Sim()->getCfg()->getInt("perf_model/dram/ddr/dram_page_size"))
   , m_dram_page_size_log2 (floorLog2(m_dram_page_size))
   // Bandwidth: MT/s * bits/transfer / 1000 = bits/ns
   , m_bus_bandwidth       (m_dram_speed * m_data_bus_width / 1000)
   // Timing parameters (all in nanoseconds, converted to SubsecondTime)
   , m_bank_keep_open      (SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_keep_open")))
   , m_bank_open_delay     (SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_open_delay")))
   , m_bank_close_delay    (SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/bank_close_delay")))
   , m_dram_access_cost    (SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/access_cost")))
   , m_intercommand_delay  (SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay")))
   , m_intercommand_delay_short(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay_short")))
   , m_intercommand_delay_long(SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/intercommand_delay_long")))
   , m_controller_delay    (SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/controller_delay")))
   , m_refresh_interval    (SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/refresh_interval")))
   , m_refresh_length      (SubsecondTime::NS() * static_cast<uint64_t>(Sim()->getCfg()->getFloat("perf_model/dram/ddr/refresh_length")))
   , m_localdram_size      (Sim()->getCfg()->getInt("perf_model/dram/localdram_size"))
   // Bank state array
   , m_banks               (m_total_banks)
   // Statistics counters - Overall
   , m_page_hits           (0)
   , m_page_empty          (0)
   , m_page_closing        (0)
   , m_page_miss           (0)
   // Statistics - Data vs Metadata breakdown
   , m_page_hits_data      (0)
   , m_page_hits_metadata  (0)
   , m_page_empty_data     (0)
   , m_page_empty_metadata (0)
   , m_page_closing_data   (0)
   , m_page_closing_metadata(0)
   , m_page_miss_data      (0)
   , m_page_miss_metadata  (0)
   // Conflict statistics
   , m_page_conflict_metadata_to_data(0)
   , m_page_conflict_data_to_metadata(0)
   , m_page_conflict_data_to_data(0)
   , m_page_conflict_metadata_to_metadata(0)
   // Row utilization statistics
   , m_total_columns_accessed_data(0)
   , m_total_columns_accessed_metadata(0)
   , m_total_rows_closed_data(0)
   , m_total_rows_closed_metadata(0)
   , m_total_row_lifetime_data_ns(0)
   , m_total_row_lifetime_metadata_ns(0)
   , m_total_accesses_per_row_data(0)
   , m_total_accesses_per_row_metadata(0)
   // Bank distribution
   , m_bank_accesses_data(m_total_banks, 0)
   , m_bank_accesses_metadata(m_total_banks, 0)
   // Temporal statistics
   , m_received_request_from_the_past(0)
   , m_received_request_from_the_unknown_past(0)
   , m_received_request_from_the_present(0)
   , m_total_queueing_delay(SubsecondTime::Zero())
   , m_total_access_latency(SubsecondTime::Zero())
   , m_total_access_latency_data(SubsecondTime::Zero())
   , m_total_access_latency_metadata(SubsecondTime::Zero())
   , m_total_accesses_data(0)
   , m_total_accesses_metadata(0)
   // Bank conflict delay tracking
   , m_total_bank_conflict_delay_data(SubsecondTime::Zero())
   , m_total_bank_conflict_delay_metadata(SubsecondTime::Zero())
   , m_bank_conflicts_data(0)
   , m_bank_conflicts_metadata(0)
   , m_total_bank_pending_wait_data(SubsecondTime::Zero())
   , m_total_rank_avail_delay_data(SubsecondTime::Zero())
   , m_total_bank_group_avail_delay_data(SubsecondTime::Zero())
   , m_total_dram_bus_queue_delay_data(SubsecondTime::Zero())
   // Inter-arrival time tracking
   , m_last_access_time_data(SubsecondTime::Zero())
   , m_last_access_time_metadata(SubsecondTime::Zero())
   , m_total_inter_arrival_time_data(SubsecondTime::Zero())
   , m_total_inter_arrival_time_metadata(SubsecondTime::Zero())
   , m_inter_arrival_count_data(0)
   , m_inter_arrival_count_metadata(0)
   // Burst length tracking
   , m_current_burst_length(0)
   , m_total_burst_length(0)
   , m_burst_count(0)
   , m_max_burst_length(0)
   // Separate burst tracking for data vs metadata
   , m_current_burst_length_data(0)
   , m_current_burst_length_metadata(0)
   , m_total_burst_length_data(0)
   , m_total_burst_length_metadata(0)
   , m_burst_count_data(0)
   , m_burst_count_metadata(0)
   , m_max_burst_length_data(0)
   , m_max_burst_length_metadata(0)
   , m_last_access_was_metadata(false)
   // Row open duration tracking
   , m_total_row_open_duration_data(SubsecondTime::Zero())
   , m_total_row_open_duration_metadata(SubsecondTime::Zero())
   // Eviction penalty tracking
   , m_total_eviction_penalty_metadata_to_data(SubsecondTime::Zero())
   , m_total_eviction_penalty_data_to_metadata(SubsecondTime::Zero())
   , m_evictions_metadata_to_data(0)
   , m_evictions_data_to_metadata(0)
   // Queue depth tracking
   , m_total_queue_depth_at_access_data(0)
   , m_total_queue_depth_at_access_metadata(0)
   // Hot row tracking
   , m_hot_row_accesses_data(0)
   , m_hot_row_accesses_metadata(0)
   , m_cold_row_accesses_data(0)
   , m_cold_row_accesses_metadata(0)
   // Same-bank co-residency
   , m_bank_has_seen_data(m_total_banks, false)
   , m_bank_has_seen_metadata(m_total_banks, false)
   , m_banks_with_mixed_access(0)
   , m_same_bank_data_after_metadata(0)
   , m_same_bank_metadata_after_data(0)
   // PTW tracking
   , m_ptw_logging_enabled(false)
   , m_ptw_count(0)
   , m_ptw_single_bank_metadata(0)
   , m_ptw_multi_bank_metadata(0)
   , m_ptw_data_same_bank_as_metadata(0)
   , m_total_ptw_metadata_accesses(0)
   , m_total_ptw_unique_banks(0)
   // Policy flags
   , constant_time_policy(Sim()->getCfg()->getBool("perf_model/dram/ddr/constant_time_policy"))
   , selective_constant_time_policy(Sim()->getCfg()->getBool("perf_model/dram/ddr/selective_constant_time_policy"))
   , open_row_policy(Sim()->getCfg()->getBool("perf_model/dram/ddr/open_row_policy"))
   // Logging
   , m_log("DRAM_DETAILED", core_id, DEBUG_DRAM_DETAILED)
   , m_csv_logging_enabled(false)
{
   // Initialize PTW trace
   m_current_ptw.active = false;
   m_current_ptw.data_bank = 0;
   m_current_ptw.data_page = 0;
   m_current_ptw.data_time = SubsecondTime::Zero();
   m_current_ptw.start_time = SubsecondTime::Zero();
   m_current_ptw.ptw_id = 0;
   m_last_ptw_id = 0;
   m_data_access_ptw_id = 0;  // 0 means no associated PTW
   
   // Initialize PTW depth histogram
   for (UInt32 i = 0; i < PTW_DEPTH_BUCKETS; ++i) {
      m_ptw_depth_histogram[i] = 0;
   }

   // Open CSV file for latency logging if CSV logging is enabled (DEBUG_DRAM_CSV flag)
#if DEBUG_DRAM_CSV
   {
#ifdef ENABLE_DRAM_LATENCY_CSV
      // WARNING: This log can become extremely large (one line per DRAM access)!
      std::string csv_filename = "dram_latency_core" + std::to_string(core_id) + ".csv";
      std::string output_dir = std::string(Sim()->getConfig()->getOutputDirectory().c_str());

      csv_filename = output_dir + "/" + csv_filename;
      m_latency_csv.open(csv_filename.c_str());
      if (m_latency_csv.is_open())
      {
         m_csv_logging_enabled = true;
         // Write CSV header (ptw_id = 0 means not associated with any PTW)
         m_latency_csv << "timestamp_ns,address,is_metadata,latency_ns,page_hit,bank,rank,channel,page,ptw_id" << std::endl;
      }
#endif
      
      // Open PTW trace CSV file
      std::string ptw_csv_filename = "dram_ptw_traces_core" + std::to_string(core_id) + ".csv";
      std::string output_dir_ptw = std::string(Sim()->getConfig()->getOutputDirectory().c_str());
      ptw_csv_filename = output_dir_ptw + "/" + ptw_csv_filename;
      m_ptw_csv.open(ptw_csv_filename.c_str());
      if (m_ptw_csv.is_open())
      {
         m_ptw_logging_enabled = true;
         // Write CSV header: metadata_banks is semicolon-separated list
         m_ptw_csv << "ptw_id,start_time_ns,end_time_ns,num_metadata_accesses,metadata_banks,metadata_pages,metadata_depths,metadata_levels,data_bank,data_page,unique_banks,data_in_metadata_bank" << std::endl;
      }
   }
#endif

   // Initialize row utilization histograms
   for (UInt32 i = 0; i < NUM_UTIL_BUCKETS; ++i)
   {
      m_row_util_histogram_data[i] = 0;
      m_row_util_histogram_metadata[i] = 0;
   }

   String name("dram" + m_suffix);
   String ddr_name("ddr" + m_suffix);

   // Log configuration at initialization
   m_log.info("=== DRAM Detailed Model Initialization ===");
   m_log.info("Address mapping:", m_address_mapping->getName());
   m_log.info("Organization: banks=", m_num_banks, "ranks=", m_num_ranks,
              "channels=", m_num_channels, "bank_groups=", m_num_bank_groups);
   m_log.info("Total: banks=", m_total_banks, "ranks=", m_total_ranks,
              "bank_groups=", m_total_bank_groups);
   m_log.info("Page size:", m_dram_page_size, "bytes, DRAM speed:", m_dram_speed, "MT/s");
   m_log.info("Timing (ns): open=", m_bank_open_delay.getNS(),
              "close=", m_bank_close_delay.getNS(),
              "access=", m_dram_access_cost.getNS());

   // Create per-channel queue models for data bus contention
   if (Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled"))
   {
      for (UInt32 channel = 0; channel < m_num_channels; ++channel)
      {
         m_queue_model.push_back(QueueModel::create(
            name + "-queue-" + itostr(channel), core_id,
            Sim()->getCfg()->getString("perf_model/dram/queue_model/type"),
            m_bus_bandwidth.getRoundedLatency(8))); // 8 bytes = 64-bit cache line transfer
      }
   }

   // Register statistics
   registerStatsMetric(name, core_id, "total-access-latency", &m_total_access_latency);
   registerStatsMetric(name, core_id, "total-access-latency-data", &m_total_access_latency_data);
   registerStatsMetric(name, core_id, "total-access-latency-metadata", &m_total_access_latency_metadata);
   registerStatsMetric(name, core_id, "total-accesses-data", &m_total_accesses_data);
   registerStatsMetric(name, core_id, "total-accesses-metadata", &m_total_accesses_metadata);

   // Create per-rank command bus availability models
   for (UInt32 rank = 0; rank < m_total_ranks; ++rank)
   {
      m_rank_avail.push_back(QueueModel::create(
         name + "-rank-" + itostr(rank), core_id, "history_list",
         (m_num_bank_groups > 1) ? m_intercommand_delay_short : m_intercommand_delay));
   }

   // Create per-bank-group timing models (for tCCD_L vs tCCD_S)
   for (UInt32 group = 0; group < m_total_bank_groups; ++group)
   {
      m_bank_group_avail.push_back(QueueModel::create(
         name + "-bank-group-" + itostr(group), core_id, "history_list",
         m_intercommand_delay_long));
   }

   // Initialize bank state
   for (UInt32 bank = 0; bank < m_total_banks; ++bank)
   {
      m_banks[bank].core_id = -1;
      m_banks[bank].open_page = -1;
      m_banks[bank].t_avail = SubsecondTime::Zero();
      m_banks[bank].max_time = SubsecondTime::Zero();
      m_banks[bank].max_page = -1;
      m_banks[bank].open_page_type = NOT_METADATA;
      // Row buffer utilization tracking
      m_banks[bank].columns_accessed.reset();
      m_banks[bank].data_accesses = 0;
      m_banks[bank].metadata_accesses = 0;
      m_banks[bank].open_time = SubsecondTime::Zero();
      m_banks[bank].close_time = SubsecondTime::Zero();
   }

   // Validate configuration
   LOG_ASSERT_ERROR(cache_block_size == 64, "Hardcoded for 64-byte cache lines");

   // Register row buffer statistics
   registerStatsMetric(ddr_name, core_id, "page-hits", &m_page_hits);
   registerStatsMetric(ddr_name, core_id, "page-empty", &m_page_empty);
   registerStatsMetric(ddr_name, core_id, "page-closing", &m_page_closing);
   registerStatsMetric(ddr_name, core_id, "page-miss", &m_page_miss);

   // Register per-type (data vs metadata) row buffer statistics
   registerStatsMetric(ddr_name, core_id, "page-hits-data", &m_page_hits_data);
   registerStatsMetric(ddr_name, core_id, "page-hits-metadata", &m_page_hits_metadata);
   registerStatsMetric(ddr_name, core_id, "page-empty-data", &m_page_empty_data);
   registerStatsMetric(ddr_name, core_id, "page-empty-metadata", &m_page_empty_metadata);
   registerStatsMetric(ddr_name, core_id, "page-closing-data", &m_page_closing_data);
   registerStatsMetric(ddr_name, core_id, "page-closing-metadata", &m_page_closing_metadata);
   registerStatsMetric(ddr_name, core_id, "page-miss-data", &m_page_miss_data);
   registerStatsMetric(ddr_name, core_id, "page-miss-metadata", &m_page_miss_metadata);

   // Register row buffer utilization statistics
   registerStatsMetric(ddr_name, core_id, "total-row-closes-data", &m_total_rows_closed_data);
   registerStatsMetric(ddr_name, core_id, "total-row-closes-metadata", &m_total_rows_closed_metadata);
   registerStatsMetric(ddr_name, core_id, "total-columns-accessed-data", &m_total_columns_accessed_data);
   registerStatsMetric(ddr_name, core_id, "total-columns-accessed-metadata", &m_total_columns_accessed_metadata);

   // Register utilization histogram (data rows) - 7 buckets: 1, 2-4, 5-8, 9-16, 17-32, 33-64, 65+
   registerStatsMetric(ddr_name, core_id, "util-histogram-1-data", &m_row_util_histogram_data[0]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-2-4-data", &m_row_util_histogram_data[1]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-5-8-data", &m_row_util_histogram_data[2]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-9-16-data", &m_row_util_histogram_data[3]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-17-32-data", &m_row_util_histogram_data[4]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-33-64-data", &m_row_util_histogram_data[5]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-65plus-data", &m_row_util_histogram_data[6]);

   // Register utilization histogram (metadata rows)
   registerStatsMetric(ddr_name, core_id, "util-histogram-1-metadata", &m_row_util_histogram_metadata[0]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-2-4-metadata", &m_row_util_histogram_metadata[1]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-5-8-metadata", &m_row_util_histogram_metadata[2]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-9-16-metadata", &m_row_util_histogram_metadata[3]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-17-32-metadata", &m_row_util_histogram_metadata[4]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-33-64-metadata", &m_row_util_histogram_metadata[5]);
   registerStatsMetric(ddr_name, core_id, "util-histogram-65plus-metadata", &m_row_util_histogram_metadata[6]);

   // Register metadata/data conflict statistics (for security analysis)
   registerStatsMetric(ddr_name, core_id, "page-conflict-data-to-metadata", &m_page_conflict_data_to_metadata);
   registerStatsMetric(ddr_name, core_id, "page-conflict-metadata-to-data", &m_page_conflict_metadata_to_data);
   registerStatsMetric(ddr_name, core_id, "page-conflict-metadata-to-metadata", &m_page_conflict_metadata_to_metadata);
   registerStatsMetric(ddr_name, core_id, "page-conflict-data-to-data", &m_page_conflict_data_to_data);

   // Register out-of-order request statistics
   registerStatsMetric(name, core_id, "received-request-from-the-past", &m_received_request_from_the_past);
   registerStatsMetric(name, core_id, "received-request-from-the-unknown-past", &m_received_request_from_the_unknown_past);
   registerStatsMetric(name, core_id, "received-request-from-present", &m_received_request_from_the_present);

   // Register bank conflict delay statistics
   registerStatsMetric(ddr_name, core_id, "total-bank-conflict-delay-data", &m_total_bank_conflict_delay_data);
   registerStatsMetric(ddr_name, core_id, "total-bank-conflict-delay-metadata", &m_total_bank_conflict_delay_metadata);
   registerStatsMetric(ddr_name, core_id, "total-bank-pending-wait-data",      &m_total_bank_pending_wait_data);
   registerStatsMetric(ddr_name, core_id, "total-rank-avail-delay-data",       &m_total_rank_avail_delay_data);
   registerStatsMetric(ddr_name, core_id, "total-bank-group-avail-delay-data", &m_total_bank_group_avail_delay_data);
   registerStatsMetric(ddr_name, core_id, "total-dram-bus-queue-delay-data",   &m_total_dram_bus_queue_delay_data);
   registerStatsMetric(ddr_name, core_id, "bank-conflicts-data", &m_bank_conflicts_data);
   registerStatsMetric(ddr_name, core_id, "bank-conflicts-metadata", &m_bank_conflicts_metadata);

   // Register inter-arrival time statistics
   registerStatsMetric(ddr_name, core_id, "total-inter-arrival-time-data", &m_total_inter_arrival_time_data);
   registerStatsMetric(ddr_name, core_id, "total-inter-arrival-time-metadata", &m_total_inter_arrival_time_metadata);
   registerStatsMetric(ddr_name, core_id, "inter-arrival-count-data", &m_inter_arrival_count_data);
   registerStatsMetric(ddr_name, core_id, "inter-arrival-count-metadata", &m_inter_arrival_count_metadata);

   // Register burst length statistics
   registerStatsMetric(ddr_name, core_id, "total-burst-length", &m_total_burst_length);
   registerStatsMetric(ddr_name, core_id, "burst-count", &m_burst_count);
   registerStatsMetric(ddr_name, core_id, "max-burst-length", &m_max_burst_length);
   
   // Register separate burst length statistics for data vs metadata
   registerStatsMetric(ddr_name, core_id, "total-burst-length-data", &m_total_burst_length_data);
   registerStatsMetric(ddr_name, core_id, "total-burst-length-metadata", &m_total_burst_length_metadata);
   registerStatsMetric(ddr_name, core_id, "burst-count-data", &m_burst_count_data);
   registerStatsMetric(ddr_name, core_id, "burst-count-metadata", &m_burst_count_metadata);
   registerStatsMetric(ddr_name, core_id, "max-burst-length-data", &m_max_burst_length_data);
   registerStatsMetric(ddr_name, core_id, "max-burst-length-metadata", &m_max_burst_length_metadata);

   // Register row open duration statistics
   registerStatsMetric(ddr_name, core_id, "total-row-open-duration-data", &m_total_row_open_duration_data);
   registerStatsMetric(ddr_name, core_id, "total-row-open-duration-metadata", &m_total_row_open_duration_metadata);

   // Register eviction penalty statistics
   registerStatsMetric(ddr_name, core_id, "total-eviction-penalty-metadata-to-data", &m_total_eviction_penalty_metadata_to_data);
   registerStatsMetric(ddr_name, core_id, "total-eviction-penalty-data-to-metadata", &m_total_eviction_penalty_data_to_metadata);
   registerStatsMetric(ddr_name, core_id, "evictions-metadata-to-data", &m_evictions_metadata_to_data);
   registerStatsMetric(ddr_name, core_id, "evictions-data-to-metadata", &m_evictions_data_to_metadata);

   // Register queue depth statistics
   registerStatsMetric(ddr_name, core_id, "total-queue-depth-at-access-data", &m_total_queue_depth_at_access_data);
   registerStatsMetric(ddr_name, core_id, "total-queue-depth-at-access-metadata", &m_total_queue_depth_at_access_metadata);

   // Register hot row statistics
   registerStatsMetric(ddr_name, core_id, "hot-row-accesses-data", &m_hot_row_accesses_data);
   registerStatsMetric(ddr_name, core_id, "hot-row-accesses-metadata", &m_hot_row_accesses_metadata);
   registerStatsMetric(ddr_name, core_id, "cold-row-accesses-data", &m_cold_row_accesses_data);
   registerStatsMetric(ddr_name, core_id, "cold-row-accesses-metadata", &m_cold_row_accesses_metadata);

   // Register same-bank co-residency statistics
   registerStatsMetric(ddr_name, core_id, "banks-with-mixed-access", &m_banks_with_mixed_access);
   registerStatsMetric(ddr_name, core_id, "same-bank-data-after-metadata", &m_same_bank_data_after_metadata);
   registerStatsMetric(ddr_name, core_id, "same-bank-metadata-after-data", &m_same_bank_metadata_after_data);

   // Register PTW (Page Table Walk) statistics
   registerStatsMetric(ddr_name, core_id, "ptw-count", &m_ptw_count);
   registerStatsMetric(ddr_name, core_id, "ptw-single-bank-metadata", &m_ptw_single_bank_metadata);
   registerStatsMetric(ddr_name, core_id, "ptw-multi-bank-metadata", &m_ptw_multi_bank_metadata);
   registerStatsMetric(ddr_name, core_id, "ptw-data-same-bank-as-metadata", &m_ptw_data_same_bank_as_metadata);
   registerStatsMetric(ddr_name, core_id, "total-ptw-metadata-accesses", &m_total_ptw_metadata_accesses);
   registerStatsMetric(ddr_name, core_id, "total-ptw-unique-banks", &m_total_ptw_unique_banks);
   
   // PTW depth histogram (1, 2, 3, 4, 5+ metadata accesses per PTW)
   registerStatsMetric(ddr_name, core_id, "ptw-depth-1", &m_ptw_depth_histogram[0]);
   registerStatsMetric(ddr_name, core_id, "ptw-depth-2", &m_ptw_depth_histogram[1]);
   registerStatsMetric(ddr_name, core_id, "ptw-depth-3", &m_ptw_depth_histogram[2]);
   registerStatsMetric(ddr_name, core_id, "ptw-depth-4", &m_ptw_depth_histogram[3]);
   registerStatsMetric(ddr_name, core_id, "ptw-depth-5plus", &m_ptw_depth_histogram[4]);
}

//=============================================================================
// Destructor
//=============================================================================

DramPerfModelDetailed::~DramPerfModelDetailed()
{
   // Close CSV files if they were opened
   if (m_latency_csv.is_open())
   {
      m_latency_csv.close();
   }
   if (m_ptw_csv.is_open())
   {
      m_ptw_csv.close();
   }

   for (auto* model : m_queue_model)
      delete model;

   for (auto* model : m_rank_avail)
      delete model;

   for (auto* model : m_bank_group_avail)
      delete model;
}

//=============================================================================
// Row Buffer Utilization Tracking
//=============================================================================

/**
 * @brief Record row buffer utilization when a row is closed
 * 
 * This function calculates how many unique columns were accessed
 * before the row was closed due to a conflict. It updates
 * histograms and counters separately for data and metadata rows.
 * 
 * Histogram buckets: 1, 2-4, 5-8, 9-16, 17-32, 33-64, 65+
 * 
 * @param bank      Bank index where the row is being closed
 * @param is_metadata  Whether the row being closed was a metadata row
 */
void DramPerfModelDetailed::recordRowClose(UInt32 bank, bool is_metadata)
{
   BankInfo& info = m_banks[bank];
   
   // Count how many unique columns were accessed
   size_t columns_used = info.columns_accessed.count();
   
   if (columns_used == 0) {
      // No columns accessed - shouldn't happen but handle gracefully
      return;
   }
   
   // Determine histogram bucket based on column count
   // Buckets: 0=1, 1=2-4, 2=5-8, 3=9-16, 4=17-32, 5=33-64, 6=65+
   int bucket;
   if (columns_used == 1) {
      bucket = 0;
   } else if (columns_used <= 4) {
      bucket = 1;
   } else if (columns_used <= 8) {
      bucket = 2;
   } else if (columns_used <= 16) {
      bucket = 3;
   } else if (columns_used <= 32) {
      bucket = 4;
   } else if (columns_used <= 64) {
      bucket = 5;
   } else {
      bucket = 6;
   }
   
   // Update appropriate histogram and counters
   if (is_metadata) {
      m_row_util_histogram_metadata[bucket]++;
      m_total_rows_closed_metadata++;
      m_total_columns_accessed_metadata += columns_used;
      m_total_accesses_per_row_metadata += info.metadata_accesses;
   } else {
      m_row_util_histogram_data[bucket]++;
      m_total_rows_closed_data++;
      m_total_columns_accessed_data += columns_used;
      m_total_accesses_per_row_data += info.data_accesses;
   }
   
   // Reset tracking for next row
   info.columns_accessed.reset();
   info.data_accesses = 0;
   info.metadata_accesses = 0;
}

//=============================================================================
// PTW (Page Table Walk) Tracking
//=============================================================================

/**
 * @brief Record a completed PTW trace
 * 
 * Called when a data access follows metadata accesses, indicating
 * the end of a page table walk. Records statistics and optionally
 * logs the trace to CSV.
 */
void DramPerfModelDetailed::recordPTWComplete()
{
   if (!m_current_ptw.active || m_current_ptw.metadata_banks.empty()) {
      return;
   }
   
   m_ptw_count++;
   
   // Count unique banks used for metadata
   std::set<UInt32> unique_metadata_banks(m_current_ptw.metadata_banks.begin(), 
                                           m_current_ptw.metadata_banks.end());
   UInt32 num_unique_banks = unique_metadata_banks.size();
   
   m_total_ptw_metadata_accesses += m_current_ptw.metadata_banks.size();
   m_total_ptw_unique_banks += num_unique_banks;
   
   // Track single vs multi-bank PTWs
   if (num_unique_banks == 1) {
      m_ptw_single_bank_metadata++;
   } else {
      m_ptw_multi_bank_metadata++;
   }
   
   // Check if data bank was also used for metadata
   if (unique_metadata_banks.count(m_current_ptw.data_bank) > 0) {
      m_ptw_data_same_bank_as_metadata++;
   }
   
   // Update PTW depth histogram
   UInt32 depth = m_current_ptw.metadata_banks.size();
   if (depth >= PTW_DEPTH_BUCKETS) {
      m_ptw_depth_histogram[PTW_DEPTH_BUCKETS - 1]++;
   } else {
      m_ptw_depth_histogram[depth - 1]++;
   }
   
   // Log to CSV if enabled
   if (m_ptw_logging_enabled)
   {
      // Build semicolon-separated list of metadata banks, pages, depths, levels
      std::ostringstream banks_ss, pages_ss, depths_ss, levels_ss;
      for (size_t i = 0; i < m_current_ptw.metadata_banks.size(); ++i) {
         if (i > 0) {
            banks_ss << ";";
            pages_ss << ";";
            depths_ss << ";";
            levels_ss << ";";
         }
         banks_ss << m_current_ptw.metadata_banks[i];
         pages_ss << m_current_ptw.metadata_pages[i];
         depths_ss << (i < m_current_ptw.metadata_depths.size() ? m_current_ptw.metadata_depths[i] : 0);
         levels_ss << (i < m_current_ptw.metadata_levels.size() ? m_current_ptw.metadata_levels[i] : 0);
      }
      
      m_ptw_csv << m_current_ptw.ptw_id << ","
                << m_current_ptw.start_time.getNS() << ","
                << m_current_ptw.data_time.getNS() << ","
                << m_current_ptw.metadata_banks.size() << ","
                << banks_ss.str() << ","
                << pages_ss.str() << ","
                << depths_ss.str() << ","
                << levels_ss.str() << ","
                << m_current_ptw.data_bank << ","
                << m_current_ptw.data_page << ","
                << num_unique_banks << ","
                << (unique_metadata_banks.count(m_current_ptw.data_bank) > 0 ? 1 : 0)
                << std::endl;
   }
   
   // Reset PTW trace
   m_current_ptw.metadata_banks.clear();
   m_current_ptw.metadata_pages.clear();
   m_current_ptw.metadata_times.clear();
   m_current_ptw.metadata_depths.clear();
   m_current_ptw.metadata_levels.clear();
   m_current_ptw.active = false;
}

//=============================================================================
// Bank Busy Interval Management
//=============================================================================

/**
 * @brief Check if a request falls within a bank's busy interval
 * 
 * This handles out-of-order requests that may arrive with timestamps
 * before the bank's current state. It searches through recorded busy
 * intervals to find the correct availability time.
 * 
 * @param page      Row/page being accessed
 * @param pkt_time  Timestamp of the request
 * @param bank      Bank index
 * @return Pair of (available_time, interval_info) for the request
 */
std::pair<SubsecondTime, DramPerfModelDetailed::IntervalNode>
DramPerfModelDetailed::fallsWithinInterval(UInt64 page, SubsecondTime pkt_time, IntPtr bank)
{
   if (m_banks[bank].m_bank_busy_intervals.empty())
   {
      // No recorded intervals - bank is immediately available
      return {pkt_time, IntervalNode()};
   }

   // Copy the priority queue to iterate without modifying original
   std::priority_queue<IntervalNode> temp = m_banks[bank].m_bank_busy_intervals;

   IntervalNode last_interval;
   last_interval.start_time = SubsecondTime::Zero();
   last_interval.end_time = SubsecondTime::Zero();
   last_interval.open_page = -1;

   while (!temp.empty())
   {
      IntervalNode interval = temp.top();
      temp.pop();

      if (interval.start_time > pkt_time)
      {
         // Found an interval that starts after pkt_time
         if (last_interval.open_page == static_cast<IntPtr>(-1))
         {
            // Request is from before any recorded interval
            m_received_request_from_the_unknown_past++;
            m_log.trace("Received request from unknown past:", pkt_time.getNS(), "ns");
         }
         else
         {
            m_received_request_from_the_past++;
            m_log.trace("Received request from past:", pkt_time.getNS(), "ns");
         }
         return {pkt_time, last_interval};
      }

      if (interval.start_time <= pkt_time && interval.end_time >= pkt_time)
      {
         // Request overlaps with this interval - must wait
         m_received_request_from_the_past++;
         SubsecondTime next_avail = interval.end_time + SubsecondTime::NS(1);
         m_log.trace("Request overlaps interval, waiting until:", next_avail.getNS(), "ns");
         return {next_avail, interval};
      }

      last_interval = interval;
   }

   // No intervals overlap - bank is available at pkt_time
   m_received_request_from_the_present++;
   m_log.trace("Received request from present:", pkt_time.getNS(), "ns");
   return {pkt_time, last_interval};
}

/**
 * @brief Remove the oldest busy interval from a bank
 * Called when the interval queue exceeds the maximum size limit.
 */
void DramPerfModelDetailed::cleanupBusyIntervals(IntPtr bank)
{
   m_banks[bank].m_bank_busy_intervals.pop();
}

/**
 * @brief Debug helper to print all busy intervals for a bank
 */
void DramPerfModelDetailed::printInterval(std::priority_queue<IntervalNode> intervals)
{
   if (!m_log.isEnabled(SimLog::LEVEL_TRACE)) {
      (void)intervals;
      return;
   }

   std::priority_queue<IntervalNode> temp = intervals;
   while (!temp.empty())
   {
      IntervalNode interval = temp.top();
      temp.pop();
      m_log.trace("Interval: [", interval.start_time.getNS(), "ns -",
                  interval.end_time.getNS(), "ns] Page:", interval.open_page);
   }
}

//=============================================================================
// Main Access Latency Calculation
//=============================================================================

/**
 * @brief Calculate DRAM access latency with full timing model
 * 
 * Models the complete DDR access including:
 * 1. Controller pipeline delay
 * 2. Refresh interference
 * 3. Row buffer hit/miss/conflict handling
 * 4. Bank, rank, and bank group timing constraints
 * 5. Data bus contention
 * 
 * @param pkt_time     Request arrival time
 * @param pkt_size     Size of the request in bytes
 * @param requester    Core ID making the request
 * @param address      Physical memory address
 * @param access_type  Read or write
 * @param perf         Performance tracking object
 * @param is_metadata  True if accessing page table/metadata (for conflict tracking)
 * @return Total access latency
 */
SubsecondTime DramPerfModelDetailed::getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size,
                                                       core_id_t requester, IntPtr address,
                                                       DramCntlrInterface::access_t access_type,
                                                       ShmemPerf *perf, bool is_metadata)
{
   (void)access_type; // Currently unused - could be used for read/write differentiation

   // Parse address into DRAM components using the configured mapping
   DramAddress addr = m_address_mapping->parse(address);

   m_log.debug("Access: page=", addr.page, "bank=", addr.bank, "rank=", addr.rank, "channel=", addr.channel);

   SubsecondTime t_now = pkt_time;
   perf->updateTime(t_now);

   //--- Track inter-arrival time ---
   if (is_metadata) {
      if (m_last_access_time_metadata != SubsecondTime::Zero() && pkt_time > m_last_access_time_metadata) {
         m_total_inter_arrival_time_metadata += (pkt_time - m_last_access_time_metadata);
         m_inter_arrival_count_metadata++;
      }
      m_last_access_time_metadata = pkt_time;
   } else {
      if (m_last_access_time_data != SubsecondTime::Zero() && pkt_time > m_last_access_time_data) {
         m_total_inter_arrival_time_data += (pkt_time - m_last_access_time_data);
         m_inter_arrival_count_data++;
      }
      m_last_access_time_data = pkt_time;
   }

   //--- Stage 1: Controller Pipeline Delay ---
   t_now += m_controller_delay;
   perf->updateTime(t_now, ShmemPerf::DRAM_CNTLR);

   //--- Stage 2: Check for Refresh Interference ---
   if (m_refresh_interval != SubsecondTime::Zero())
   {
      SubsecondTime refresh_base = (t_now.getPS() / m_refresh_interval.getPS()) * m_refresh_interval;
      if (t_now - refresh_base < m_refresh_length)
      {
         t_now = refresh_base + m_refresh_length;
         perf->updateTime(t_now, ShmemPerf::DRAM_REFRESH);
      }
   }

   //--- Stage 3: Bank and Row Buffer Handling ---
   // Calculate flat bank index: crb = channel * ranks * banks + rank * banks + bank
   UInt64 crb = (addr.channel * m_num_ranks * m_num_banks) + (addr.rank * m_num_banks) + addr.bank;
   LOG_ASSERT_ERROR(crb < m_total_banks, "Bank index out of bounds");
   BankInfo &bank_info = m_banks[crb];

   //--- Track same-bank co-residency ---
   if (is_metadata) {
      if (m_bank_has_seen_data[crb] && !m_bank_has_seen_metadata[crb]) {
         // First metadata access to a bank that has seen data
         m_banks_with_mixed_access++;
      }
      if (m_bank_has_seen_data[crb]) {
         m_same_bank_metadata_after_data++;
      }
      m_bank_has_seen_metadata[crb] = true;
   } else {
      if (m_bank_has_seen_metadata[crb] && !m_bank_has_seen_data[crb]) {
         // First data access to a bank that has seen metadata
         m_banks_with_mixed_access++;
      }
      if (m_bank_has_seen_metadata[crb]) {
         m_same_bank_data_after_metadata++;
      }
      m_bank_has_seen_data[crb] = true;
   }

   //--- Track hot/cold row accesses ---
   UInt64 full_page_id = (crb << 32) | addr.page;  // Combine bank and page for unique ID
   auto page_it = m_page_access_count.find(full_page_id);
   if (page_it != m_page_access_count.end()) {
      // Hot row - seen before
      page_it->second++;
      if (is_metadata)
         m_hot_row_accesses_metadata++;
      else
         m_hot_row_accesses_data++;
   } else {
      // Cold row - first access
      if (m_page_access_count.size() < HOT_ROW_TRACKING_SIZE) {
         m_page_access_count[full_page_id] = 1;
      }
      if (is_metadata)
         m_cold_row_accesses_metadata++;
      else
         m_cold_row_accesses_data++;
   }
   //--- Track PTW (Page Table Walk) bank access patterns ---
   // Use MetadataContext for explicit PTW info if available (indexed by requester core_id)
   m_log.debug("PTW Context Valid for core", requester, ":", MetadataContext::isValid(requester));
   if (MetadataContext::isValid(requester)) {
      const MetadataInfo& ptw_info = MetadataContext::get(requester);
      
      m_log.debug("PTW Tracking: is_metadata=", ptw_info.is_metadata,
                  "ptw_id=", ptw_info.ptw_id,
                  "ptw_depth=", ptw_info.ptw_depth,
                  "ptw_level=", ptw_info.ptw_level,
                  "is_data_after_ptw=", ptw_info.is_data_after_ptw);

      if (ptw_info.is_metadata) {
         // This is a metadata (page table) access
         // Check if this is a new PTW (different ptw_id)
         if (ptw_info.ptw_id != m_last_ptw_id) {

            m_log.debug("Starting new PTW trace with ID:", ptw_info.ptw_id);
            // If previous PTW was active, complete it first
            if (m_current_ptw.active) {
               recordPTWComplete();
            }
            // Start new PTW with explicit ID
            m_current_ptw.active = true;
            m_current_ptw.ptw_id = ptw_info.ptw_id;
            m_current_ptw.start_time = pkt_time;
            m_current_ptw.metadata_banks.clear();
            m_current_ptw.metadata_pages.clear();
            m_current_ptw.metadata_times.clear();
            m_current_ptw.metadata_depths.clear();
            m_current_ptw.metadata_levels.clear();
            m_last_ptw_id = ptw_info.ptw_id;
         }

         m_log.debug("Recording PTW metadata access: bank=", crb, "page=", addr.page);
         // Add this access to the PTW trace
         m_current_ptw.metadata_banks.push_back(static_cast<UInt32>(crb));
         m_current_ptw.metadata_pages.push_back(addr.page);
         m_current_ptw.metadata_times.push_back(pkt_time);
         m_current_ptw.metadata_depths.push_back(ptw_info.ptw_depth);
         m_current_ptw.metadata_levels.push_back(ptw_info.ptw_level);
         // Track PTW ID for this metadata access
         m_data_access_ptw_id = m_current_ptw.ptw_id;
      } 
      else if (ptw_info.is_data_after_ptw) {
         // This is a data access that followed a PTW
         m_log.debug("Data access associated with PTW ID:", ptw_info.ptw_id);
         m_data_access_ptw_id = ptw_info.ptw_id;
         
         // Complete any pending PTW trace with this data access info
         if (m_current_ptw.active && m_current_ptw.ptw_id == ptw_info.ptw_id) {
            m_log.debug("Completing PTW trace with data access: bank=", crb, "page=", addr.page);
            m_current_ptw.data_bank = static_cast<UInt32>(crb);
            m_current_ptw.data_page = addr.page;
            m_current_ptw.data_time = pkt_time;
            recordPTWComplete();
         }
      }
   } 
   else {
      // No MetadataContext - regular data access not associated with any PTW
      m_data_access_ptw_id = 0;
   }

   SubsecondTime t_avail = t_now;

   // Check if this is a new "future" request or one that falls within recorded history
   if (t_now > bank_info.max_time)
   {
      // Request is in the future - update max time
      bank_info.max_time = t_now;
      bank_info.open_page = addr.page;
   }
   else
   {
      // Request may be out-of-order - check busy intervals
      auto result = fallsWithinInterval(addr.page, t_now, crb);
      t_avail = result.first;
      bank_info.t_avail = result.first;
      bank_info.open_page = result.second.open_page;
   }

   //--- Stage 4: Row Buffer Hit/Miss Logic ---
   bool is_page_hit = (bank_info.open_page == addr.page) &&
                      ((bank_info.t_avail + m_bank_keep_open) >= t_now);

   // Track column access for row buffer utilization measurement
   // Column index within the row (each cache line is 64 bytes)
   UInt32 column_index = (addr.column * 64) / 64;  // addr.column is already in cache line units
   if (column_index >= 128) column_index = 127;    // Clamp to bitset size

   if (is_page_hit)
   {
      // ROW HIT: Row buffer already contains the correct page
      if (bank_info.t_avail > t_now)
      {
         SubsecondTime pending = bank_info.t_avail - t_now;
         t_now = bank_info.t_avail;
         perf->updateTime(t_now, ShmemPerf::DRAM_BANK_PENDING);
         if (!is_metadata) m_total_bank_pending_wait_data += pending;
         m_log.trace("Row hit, bank busy until:", t_now.getNS(), "ns");
      }
      ++m_page_hits;
      
      // Track per-type hits
      if (is_metadata)
         ++m_page_hits_metadata;
      else
         ++m_page_hits_data;
      
      // Mark this column as accessed
      bank_info.columns_accessed.set(column_index);
      if (is_metadata)
         bank_info.metadata_accesses++;
      else
         bank_info.data_accesses++;
      
      // Track burst length (consecutive accesses to same row)
      m_current_burst_length++;
      
      // Track per-type burst length
      // A burst continues if same type as previous access
      if (is_metadata) {
         if (m_last_access_was_metadata) {
            m_current_burst_length_metadata++;
         } else {
            // Type changed - finalize previous data burst if any
            if (m_current_burst_length_data > 0) {
               m_total_burst_length_data += m_current_burst_length_data;
               m_burst_count_data++;
               if (m_current_burst_length_data > m_max_burst_length_data)
                  m_max_burst_length_data = m_current_burst_length_data;
               m_current_burst_length_data = 0;
            }
            m_current_burst_length_metadata = 1;
         }
      } else {
         if (!m_last_access_was_metadata) {
            m_current_burst_length_data++;
         } else {
            // Type changed - finalize previous metadata burst if any
            if (m_current_burst_length_metadata > 0) {
               m_total_burst_length_metadata += m_current_burst_length_metadata;
               m_burst_count_metadata++;
               if (m_current_burst_length_metadata > m_max_burst_length_metadata)
                  m_max_burst_length_metadata = m_current_burst_length_metadata;
               m_current_burst_length_metadata = 0;
            }
            m_current_burst_length_data = 1;
         }
      }
      m_last_access_was_metadata = is_metadata;
   }
   else
   {
      // ROW MISS: Need to close current row and open new one
      SubsecondTime conflict_delay = SubsecondTime::Zero();  // Track extra delay due to conflict
      bool is_true_conflict = false;  // Only true if we need to close an open row
      
      // End current burst and record statistics
      if (m_current_burst_length > 0) {
         m_total_burst_length += m_current_burst_length;
         m_burst_count++;
         if (m_current_burst_length > m_max_burst_length)
            m_max_burst_length = m_current_burst_length;
         m_current_burst_length = 0;
      }
      
      // End per-type bursts as well (row change ends all bursts)
      if (m_current_burst_length_data > 0) {
         m_total_burst_length_data += m_current_burst_length_data;
         m_burst_count_data++;
         if (m_current_burst_length_data > m_max_burst_length_data)
            m_max_burst_length_data = m_current_burst_length_data;
         m_current_burst_length_data = 0;
      }
      if (m_current_burst_length_metadata > 0) {
         m_total_burst_length_metadata += m_current_burst_length_metadata;
         m_burst_count_metadata++;
         if (m_current_burst_length_metadata > m_max_burst_length_metadata)
            m_max_burst_length_metadata = m_current_burst_length_metadata;
         m_current_burst_length_metadata = 0;
      }
      
      // Record row open duration (if row was open)
      if (bank_info.open_page != (UInt64)-1 && bank_info.open_time != SubsecondTime::Zero())
      {
         SubsecondTime row_duration = t_now - bank_info.open_time;
         if (bank_info.open_page_type == page_type::METADATA)
            m_total_row_open_duration_metadata += row_duration;
         else
            m_total_row_open_duration_data += row_duration;
      }
      
      // Record utilization of the row being closed (if one was open)
      if (bank_info.open_page != (UInt64)-1 && bank_info.columns_accessed.any())
      {
         bool was_metadata_row = (bank_info.open_page_type == page_type::METADATA);
         recordRowClose(crb, was_metadata_row);
      }

      // Wait for bank to become available
      if (bank_info.t_avail > t_now)
      {
         SubsecondTime pending = bank_info.t_avail - t_now;
         t_now = bank_info.t_avail;
         if (!is_metadata) m_total_bank_pending_wait_data += pending;
      }

      // Check if row is still open (needs closing) or already closed
      if (bank_info.t_avail + m_bank_keep_open >= t_now)
      {
         // Row is still open - THIS IS A TRUE CONFLICT
         is_true_conflict = true;
         
         // Track conflict type for security analysis
         if (bank_info.open_page_type == page_type::METADATA && !is_metadata) {
            ++m_page_conflict_metadata_to_data;
            m_evictions_metadata_to_data++;
         }
         else if (bank_info.open_page_type == page_type::NOT_METADATA && is_metadata) {
            ++m_page_conflict_data_to_metadata;
            m_evictions_data_to_metadata++;
         }
         else if (bank_info.open_page_type == page_type::METADATA && is_metadata)
            ++m_page_conflict_metadata_to_metadata;
         else
            ++m_page_conflict_data_to_data;

         // Add precharge (close) delay - this is the conflict penalty
         conflict_delay = m_bank_close_delay;
         t_now += m_bank_close_delay;
         ++m_page_miss;
         
         // Track per-type misses
         if (is_metadata)
            ++m_page_miss_metadata;
         else
            ++m_page_miss_data;
            
         m_log.trace("Row miss, closing row at:", t_now.getNS(), "ns");
      }
      else if (bank_info.t_avail + m_bank_keep_open + m_bank_close_delay > t_now)
      {
         // Row is being closed - wait for precharge to complete
         // This is also a conflict (we're waiting for a previous close)
         is_true_conflict = true;
         SubsecondTime wait_until = bank_info.t_avail + m_bank_keep_open + m_bank_close_delay;
         conflict_delay = wait_until - t_now;
         t_now = wait_until;
         ++m_page_closing;
         
         // Track per-type closing
         if (is_metadata)
            ++m_page_closing_metadata;
         else
            ++m_page_closing_data;
            
         m_log.trace("Row closing, wait until:", t_now.getNS(), "ns");
      }
      else
      {
         // Row already closed - NO CONFLICT, just normal activation
         // is_true_conflict remains false, conflict_delay remains Zero
         ++m_page_empty;
         
         // Track per-type empty
         if (is_metadata)
            ++m_page_empty_metadata;
         else
            ++m_page_empty_data;
      }

      // Add activate (open) delay - this is NOT part of conflict delay
      // (activation is required regardless of whether there was a conflict)
      t_now += m_bank_open_delay;
      perf->updateTime(t_now, ShmemPerf::DRAM_BANK_CONFLICT);
      
      // Track bank conflict delay only for true conflicts (row was open and needed closing)
      if (is_true_conflict) {
         if (is_metadata) {
            m_total_bank_conflict_delay_metadata += conflict_delay;
            m_bank_conflicts_metadata++;
            // Track eviction penalty for data-to-metadata evictions
            if (bank_info.open_page_type == page_type::NOT_METADATA) {
               m_total_eviction_penalty_data_to_metadata += conflict_delay;
            }
         } else {
            m_total_bank_conflict_delay_data += conflict_delay;
            m_bank_conflicts_data++;
            // Track eviction penalty for metadata-to-data evictions
            if (bank_info.open_page_type == page_type::METADATA) {
               m_total_eviction_penalty_metadata_to_data += conflict_delay;
            }
         }
      }

      // Update row buffer state
      bank_info.open_page_type = is_metadata ? page_type::METADATA : page_type::NOT_METADATA;
      bank_info.open_page = open_row_policy ? addr.page : 0;
      bank_info.open_time = t_now;
      
      // Start tracking the new row - mark first column access
      bank_info.columns_accessed.reset();
      bank_info.columns_accessed.set(column_index);
      bank_info.data_accesses = is_metadata ? 0 : 1;
      bank_info.metadata_accesses = is_metadata ? 1 : 0;
      
      // Start new burst
      m_current_burst_length = 1;
      
      // Start new per-type burst
      if (is_metadata) {
         m_current_burst_length_metadata = 1;
      } else {
         m_current_burst_length_data = 1;
      }
      m_last_access_was_metadata = is_metadata;
   }

   bank_info.core_id = requester;

   //--- Stage 5: Rank Command Bus Timing ---
   UInt64 cr = (addr.channel * m_num_ranks) + addr.rank;
   LOG_ASSERT_ERROR(cr < m_total_ranks, "Rank index out of bounds");
   SubsecondTime rank_avail_request = (m_num_bank_groups > 1) ? m_intercommand_delay_short : m_intercommand_delay;
   SubsecondTime rank_avail_delay = m_rank_avail.size()
      ? m_rank_avail[cr]->computeQueueDelay(t_now, rank_avail_request, requester)
      : SubsecondTime::Zero();
   if (!is_metadata) m_total_rank_avail_delay_data += rank_avail_delay;

   //--- Stage 6: Bank Group Timing (tCCD_L) ---
   UInt64 crbg = (addr.channel * m_num_ranks * m_num_bank_groups) + (addr.rank * m_num_bank_groups) + addr.bank_group;
   LOG_ASSERT_ERROR(crbg < m_total_bank_groups, "Bank-group index out of bounds");
   SubsecondTime group_avail_delay = m_bank_group_avail.size()
      ? m_bank_group_avail[crbg]->computeQueueDelay(t_now, m_intercommand_delay_long, requester)
      : SubsecondTime::Zero();
   if (!is_metadata) m_total_bank_group_avail_delay_data += group_avail_delay;

   //--- Stage 7: Column Access (CAS) ---
   t_now += m_dram_access_cost;
   perf->updateTime(t_now, ShmemPerf::DRAM_DEVICE);

   // Update bank availability
   bank_info.t_avail = t_now;
   if (bank_info.t_avail > bank_info.max_time)
   {
      bank_info.max_time = bank_info.t_avail;
      bank_info.max_page = addr.page;
   }

   // Record this access interval for out-of-order handling
   IntervalNode node{t_avail, t_now, static_cast<IntPtr>(addr.page)};
   bank_info.m_bank_busy_intervals.push(node);
   printInterval(bank_info.m_bank_busy_intervals);

   // Limit interval history size to prevent memory growth
   if (bank_info.m_bank_busy_intervals.size() > 100)
      cleanupBusyIntervals(crb);

   // Add the larger of rank or bank group delay
   t_now += std::max(rank_avail_delay, group_avail_delay);
   perf->updateTime(t_now, ShmemPerf::DRAM_DEVICE);

   //--- Stage 8: Data Bus Transfer ---
   SubsecondTime ddr_processing_time = m_bus_bandwidth.getRoundedLatency(8 * pkt_size);
   SubsecondTime ddr_queue_delay = m_queue_model.size()
      ? m_queue_model[addr.channel]->computeQueueDelay(t_now, ddr_processing_time, requester)
      : SubsecondTime::Zero();

   // Track queue depth at access time (approximated by queue delay)
   // Queue delay gives us an estimate of how backed up the queue was
   UInt64 approx_queue_depth = ddr_queue_delay.getNS() / ddr_processing_time.getNS();
   if (is_metadata) {
      m_total_queue_depth_at_access_metadata += approx_queue_depth;
   } else {
      m_total_queue_depth_at_access_data += approx_queue_depth;
   }

   t_now += ddr_queue_delay;
   if (!is_metadata) m_total_dram_bus_queue_delay_data += ddr_queue_delay;
   perf->updateTime(t_now, ShmemPerf::DRAM_QUEUE);

   t_now += ddr_processing_time;
   perf->updateTime(t_now, ShmemPerf::DRAM_BUS);

   // Track total latency by access type (for computing averages)
   SubsecondTime access_latency = t_now - pkt_time;
   m_total_access_latency += access_latency;
   if (is_metadata) {
      m_total_access_latency_metadata += access_latency;
      m_total_accesses_metadata++;
   } else {
      m_total_access_latency_data += access_latency;
      m_total_accesses_data++;
   }

   // Log to CSV if enabled
   if (m_csv_logging_enabled)
   {
      m_latency_csv << pkt_time.getNS() << ","
                    << std::hex << address << std::dec << ","
                    << (is_metadata ? 1 : 0) << ","
                    << access_latency.getNS() << ","
                    << (is_page_hit ? 1 : 0) << ","
                    << addr.bank << ","
                    << addr.rank << ","
                    << addr.channel << ","
                    << addr.page << ","
                    << m_data_access_ptw_id << std::endl;
   }

   m_log.debug("Access complete. Total latency:", access_latency.getNS(), "ns");

   return access_latency;
}

//=============================================================================
// Simplified Access Latency (Bus Only)
//=============================================================================

/**
 * @brief Get simplified DRAM latency (bus contention only)
 * 
 * Returns only the data bus transfer time and queue delay,
 * without modeling row buffer, bank, or timing constraints.
 * Useful for fast approximations or specific use cases.
 * 
 * @param pkt_time   Request arrival time
 * @param pkt_size   Size of the request in bytes
 * @param requester  Core ID making the request
 * @param address    Physical memory address (used for channel selection)
 * @return Bus transfer time + queue delay
 */
SubsecondTime DramPerfModelDetailed::getAccessLatencyUnmodelled(SubsecondTime pkt_time,
                                                                 UInt64 pkt_size,
                                                                 core_id_t requester,
                                                                 IntPtr address)
{
   DramAddress addr = m_address_mapping->parse(address);

   // Only model data bus transfer and contention
   SubsecondTime ddr_processing_time = m_bus_bandwidth.getRoundedLatency(8 * pkt_size);
   SubsecondTime ddr_queue_delay = m_queue_model.size()
      ? m_queue_model[addr.channel]->computeQueueDelay(pkt_time, ddr_processing_time, requester)
      : SubsecondTime::Zero();

   return ddr_processing_time + ddr_queue_delay;
}
