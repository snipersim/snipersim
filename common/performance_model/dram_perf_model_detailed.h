#ifndef __DRAM_PERF_MODEL_DETAILED_H__
#define __DRAM_PERF_MODEL_DETAILED_H__

#include "dram_perf_model.h"
#include "queue_model.h"
#include "fixed_types.h"
#include "subsecond_time.h"
#include "dram_cntlr_interface.h"
#include "address_home_lookup.h"
#include "dram_address_mapping.h"
#include "sim_log.h"

#include <vector>
#include <bitset>
#include <map>
#include <list>
#include <algorithm>
#include <queue>
#include <memory>
#include <fstream>

class DramPerfModelDetailed : public DramPerfModel
{
   private:
      const core_id_t m_core_id;
      const AddressHomeLookup* m_address_home_lookup;

      // Address mapping strategy (factory-created)
      std::unique_ptr<DramAddressMapping> m_address_mapping;

      // DRAM organization parameters
      const UInt32 m_num_banks;
      const UInt32 m_num_banks_log2;
      const UInt32 m_num_bank_groups;
      const UInt32 m_num_ranks;
      const UInt32 m_num_channels;
      const UInt32 m_total_ranks;
      const UInt32 m_banks_per_channel;
      const UInt32 m_banks_per_bank_group;
      const UInt32 m_total_banks;
      const UInt32 m_total_bank_groups;

      // Bus and speed parameters
      const UInt32 m_data_bus_width;
      const UInt32 m_dram_speed;
      const UInt32 m_dram_page_size;
      const UInt32 m_dram_page_size_log2;
      const ComponentBandwidth m_bus_bandwidth;

      // Timing parameters
      const SubsecondTime m_bank_keep_open;
      const SubsecondTime m_bank_open_delay;
      const SubsecondTime m_bank_close_delay;
      const SubsecondTime m_dram_access_cost;
      const SubsecondTime m_intercommand_delay;
      const SubsecondTime m_intercommand_delay_short;
      const SubsecondTime m_intercommand_delay_long;
      const SubsecondTime m_controller_delay;
      const SubsecondTime m_refresh_interval;
      const SubsecondTime m_refresh_length;

      const UInt32 m_localdram_size;

   
      std::vector<QueueModel*> m_queue_model;
      std::vector<QueueModel*> m_rank_avail;
      std::vector<QueueModel*> m_bank_group_avail;


      // I want to represent DRAM availability intervals using a tree structure
      // There is a tree for each bank in the system and each tree has a node for each interval of time that the bank is busy

      

      typedef enum
      {
         METADATA,
         NOT_METADATA,
         NUMBER_OF_TYPES
      }  page_type;

      struct IntervalNode {
         SubsecondTime start_time; // Start of the interval
         SubsecondTime end_time;   // End of the interval
         IntPtr open_page;         // Page that is open during this interval
         // Priority: based on interval length (example)
         bool operator<(const IntervalNode& node) const {
            return (start_time) > (node.start_time);
         }
      };


      struct BankInfo
      {
         core_id_t core_id;
         IntPtr open_page;
         SubsecondTime t_avail;
         SubsecondTime max_time;
         IntPtr max_page;
         page_type open_page_type;
         std::priority_queue<IntervalNode> m_bank_busy_intervals;

         // Row buffer utilization tracking
         std::bitset<128> columns_accessed;   // Bitmap of accessed columns (supports up to 128 columns)
         UInt32 data_accesses;                // Number of data accesses to current row
         UInt32 metadata_accesses;            // Number of metadata accesses to current row
         SubsecondTime open_time;             // When the current row was opened
         SubsecondTime close_time;            // When the current row was closed
      };

      std::vector<BankInfo> m_banks;

      //=========================================================================
      // Statistics - Overall
      //=========================================================================
      UInt64 m_page_hits;
      UInt64 m_page_empty;
      UInt64 m_page_closing;
      UInt64 m_page_miss;

      //=========================================================================
      // Statistics - Data vs Metadata breakdown
      //=========================================================================
      // Page hits by access type
      UInt64 m_page_hits_data;
      UInt64 m_page_hits_metadata;

      // Page empty (row was already closed) by access type
      UInt64 m_page_empty_data;
      UInt64 m_page_empty_metadata;

      // Page closing (row being closed) by access type
      UInt64 m_page_closing_data;
      UInt64 m_page_closing_metadata;

      // Page miss (row conflict) by access type
      UInt64 m_page_miss_data;
      UInt64 m_page_miss_metadata;

      // Conflict tracking (what was evicted vs what caused eviction)
      UInt64 m_page_conflict_metadata_to_data;
      UInt64 m_page_conflict_data_to_metadata;
      UInt64 m_page_conflict_data_to_data;
      UInt64 m_page_conflict_metadata_to_metadata;

      //=========================================================================
      // Row Buffer Utilization Statistics
      //=========================================================================
      // Histogram of columns accessed before row close (buckets: 1, 2-4, 5-8, 9-16, 17-32, 33-64, 65+)
      static const UInt32 NUM_UTIL_BUCKETS = 7;
      UInt64 m_row_util_histogram_data[NUM_UTIL_BUCKETS];
      UInt64 m_row_util_histogram_metadata[NUM_UTIL_BUCKETS];

      // Total columns accessed (for computing averages)
      UInt64 m_total_columns_accessed_data;
      UInt64 m_total_columns_accessed_metadata;
      UInt64 m_total_rows_closed_data;
      UInt64 m_total_rows_closed_metadata;

      // Row lifetime (how long rows stay open)
      UInt64 m_total_row_lifetime_data_ns;
      UInt64 m_total_row_lifetime_metadata_ns;

      // Accesses per row (reuse within same row)
      UInt64 m_total_accesses_per_row_data;
      UInt64 m_total_accesses_per_row_metadata;

      //=========================================================================
      // Bank-level distribution statistics
      //=========================================================================
      std::vector<UInt64> m_bank_accesses_data;
      std::vector<UInt64> m_bank_accesses_metadata;

      //=========================================================================
      // Temporal statistics
      //=========================================================================
      UInt64 m_received_request_from_the_past;
      UInt64 m_received_request_from_the_unknown_past;
      UInt64 m_received_request_from_the_present;

      SubsecondTime m_total_queueing_delay;
      SubsecondTime m_total_access_latency;
      SubsecondTime m_total_access_latency_data;
      SubsecondTime m_total_access_latency_metadata;
      UInt64 m_total_accesses_data;
      UInt64 m_total_accesses_metadata;

      //=========================================================================
      // Bank conflict delay tracking (separately for data/metadata)
      //=========================================================================
      SubsecondTime m_total_bank_conflict_delay_data;
      SubsecondTime m_total_bank_conflict_delay_metadata;
      UInt64 m_bank_conflicts_data;
      UInt64 m_bank_conflicts_metadata;

      // Per-stage delay accounting (debugging the source of per-access lat)
      SubsecondTime m_total_bank_pending_wait_data;     // Stage 3: bank busy wait
      SubsecondTime m_total_rank_avail_delay_data;      // Stage 5: rank command bus queue
      SubsecondTime m_total_bank_group_avail_delay_data;// Stage 6: bank-group queue (tCCD_L)
      SubsecondTime m_total_dram_bus_queue_delay_data;  // Stage 8: data bus queue (channel)

      //=========================================================================
      // Inter-arrival time tracking
      //=========================================================================
      SubsecondTime m_last_access_time_data;
      SubsecondTime m_last_access_time_metadata;
      SubsecondTime m_total_inter_arrival_time_data;
      SubsecondTime m_total_inter_arrival_time_metadata;
      UInt64 m_inter_arrival_count_data;
      UInt64 m_inter_arrival_count_metadata;

      //=========================================================================
      // Burst length tracking (consecutive accesses to same row)
      //=========================================================================
      UInt64 m_current_burst_length;
      UInt64 m_total_burst_length;
      UInt64 m_burst_count;
      UInt64 m_max_burst_length;
      
      // Separate burst tracking for data vs metadata
      UInt64 m_current_burst_length_data;
      UInt64 m_current_burst_length_metadata;
      UInt64 m_total_burst_length_data;
      UInt64 m_total_burst_length_metadata;
      UInt64 m_burst_count_data;
      UInt64 m_burst_count_metadata;
      UInt64 m_max_burst_length_data;
      UInt64 m_max_burst_length_metadata;
      bool m_last_access_was_metadata;  // Track type of previous access for burst detection

      //=========================================================================
      // Row open duration tracking
      //=========================================================================
      SubsecondTime m_total_row_open_duration_data;
      SubsecondTime m_total_row_open_duration_metadata;

      //=========================================================================
      // Metadata-induced eviction penalty tracking
      //=========================================================================
      SubsecondTime m_total_eviction_penalty_metadata_to_data;  // Penalty when metadata evicts data
      SubsecondTime m_total_eviction_penalty_data_to_metadata;  // Penalty when data evicts metadata
      UInt64 m_evictions_metadata_to_data;
      UInt64 m_evictions_data_to_metadata;

      //=========================================================================
      // Queue depth tracking
      //=========================================================================
      UInt64 m_total_queue_depth_at_access_data;
      UInt64 m_total_queue_depth_at_access_metadata;

      //=========================================================================
      // Hot row tracking (per-page access frequency)
      //=========================================================================
      static const UInt32 HOT_ROW_TRACKING_SIZE = 1024;  // Track top N hot rows
      std::map<UInt64, UInt64> m_page_access_count;       // page -> access count
      UInt64 m_hot_row_accesses_data;      // Accesses to rows that have been accessed before
      UInt64 m_hot_row_accesses_metadata;
      UInt64 m_cold_row_accesses_data;     // First access to a row
      UInt64 m_cold_row_accesses_metadata;

      //=========================================================================
      // Same-bank co-residency tracking
      //=========================================================================
      // Track if a bank has seen both data and metadata
      std::vector<bool> m_bank_has_seen_data;
      std::vector<bool> m_bank_has_seen_metadata;
      UInt64 m_banks_with_mixed_access;    // Banks that have seen both types
      UInt64 m_same_bank_data_after_metadata;   // Data access to bank that had metadata
      UInt64 m_same_bank_metadata_after_data;   // Metadata access to bank that had data

      //=========================================================================
      // PTW (Page Table Walk) Bank Access Pattern Tracking
      //=========================================================================
      // Structure to hold one PTW trace
      struct PTWTrace {
         std::vector<UInt32> metadata_banks;    // Banks accessed for page table entries
         std::vector<UInt64> metadata_pages;    // Pages accessed for page table entries  
         std::vector<SubsecondTime> metadata_times;  // Timestamps
         std::vector<UInt32> metadata_depths;   // PTW depth for each access (from MetadataContext)
         std::vector<UInt32> metadata_levels;   // PTW level/table type for each access
         UInt32 data_bank;                      // Final data bank
         UInt64 data_page;                      // Final data page
         SubsecondTime data_time;               // Data access timestamp
         SubsecondTime start_time;              // When PTW started
         UInt64 ptw_id;                         // Unique PTW ID from MMU
         bool active;                           // Is a PTW in progress?
      };
      
      PTWTrace m_current_ptw;                   // Current PTW being tracked
      UInt64 m_last_ptw_id;                     // Last seen PTW ID to detect new PTWs
      UInt64 m_data_access_ptw_id;              // PTW ID associated with current data access (0 if none)
      std::ofstream m_ptw_csv;                  // CSV file for PTW traces
      bool m_ptw_logging_enabled;
      
      // PTW statistics
      UInt64 m_ptw_count;                       // Number of completed PTWs
      UInt64 m_ptw_single_bank_metadata;        // PTWs with all metadata in same bank
      UInt64 m_ptw_multi_bank_metadata;         // PTWs with metadata across multiple banks
      UInt64 m_ptw_data_same_bank_as_metadata;  // PTWs where data bank was also used for metadata
      UInt64 m_total_ptw_metadata_accesses;     // Total metadata accesses across all PTWs
      UInt64 m_total_ptw_unique_banks;          // Total unique banks used across all PTWs
      
      // Histogram of metadata accesses per PTW (1, 2, 3, 4, 5+)
      static const UInt32 PTW_DEPTH_BUCKETS = 5;
      UInt64 m_ptw_depth_histogram[PTW_DEPTH_BUCKETS];

      bool constant_time_policy; 
      bool selective_constant_time_policy; 
      bool open_row_policy;

      // Logging
      SimLog m_log;
      std::ofstream m_latency_csv;   // CSV file for per-access latency logging
      bool m_csv_logging_enabled;

      std::pair<SubsecondTime, IntervalNode> fallsWithinInterval(UInt64 page, SubsecondTime pkt_time, IntPtr bank);
      void cleanupBusyIntervals(IntPtr bank);
      void printInterval(std::priority_queue<IntervalNode> intervals);
      void recordRowClose(UInt32 bank, bool is_metadata);
      void recordPTWComplete();                 // Record completed PTW trace

   public:
      DramPerfModelDetailed(core_id_t core_id, UInt32 cache_block_size, AddressHomeLookup* address_home_lookup, const String& suffix = "");

      ~DramPerfModelDetailed();

      SubsecondTime getAccessLatencyUnmodelled(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address);
      SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address, DramCntlrInterface::access_t access_type, ShmemPerf *perf, bool is_metadata);
};

#endif /* __DRAM_PERF_MODEL_DETAILED_H__ */