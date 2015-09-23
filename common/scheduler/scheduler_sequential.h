#ifndef __SCHEDULER_SEQUENTIAL_H
#define __SCHEDULER_SEQUENTIAL_H

#include "scheduler_pinned_base.h"
#include "simulator.h"

#include "thread_stats_manager.h"

#define MAIN_CORE 0

class SchedulerSequential : public SchedulerPinnedBase
{
   public:
      SchedulerSequential(ThreadManager *thread_manager);

      virtual void threadSetInitialAffinity(thread_id_t thread_id);

      //virtual void threadSetInitialAffinity(thread_id_t thread_id);
      virtual void threadStart(thread_id_t thread_id, SubsecondTime time);
      //virtual void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      //virtual void threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time);
      virtual void threadExit(thread_id_t thread_id, SubsecondTime time);


   private:


      std::vector<std::priority_queue<thread_id_t, std::vector<thread_id_t>, std::greater<thread_id_t>>> core_waiting_threads;
      thread_id_t last_thread;
      int current_pinball_set;
      std::vector<int> seqs;
      std::vector<int> next_thread_to_execute;
      bool verbose;
      unsigned int total_pinballs;
      unsigned int cores_working;
      String outfile;

      ThreadStatsManager::ThreadStatType l1d_load_miss_stat;
      ThreadStatsManager::ThreadStatType l1d_store_miss_stat;
      ThreadStatsManager::ThreadStatType l1i_load_miss_stat;
      ThreadStatsManager::ThreadStatType l1i_store_miss_stat;
      ThreadStatsManager::ThreadStatType l2_load_miss_stat;
      ThreadStatsManager::ThreadStatType l2_store_miss_stat;
      ThreadStatsManager::ThreadStatType l3_load_miss_stat;
      ThreadStatsManager::ThreadStatType l3_store_miss_stat;

      std::ostringstream aux_mm;

      const SubsecondTime period;

      // some aux functions
      void print_message(thread_id_t thread_id, const char * message);
      void String2IntVector(std::vector<String> from, std::vector<int> to);
      void __sim_end(SubsecondTime time);

      void results_on_screen();
      void results_on_file();

      static SInt64 hook_sim_end(UInt64 ptr, UInt64 time)
      { ((SchedulerSequential*)ptr)->__sim_end(*(subsecond_time_t*)&time); return 0; }

};

#endif // __SCHEDULER_SEQUENTIAL_H
