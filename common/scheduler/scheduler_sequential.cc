/*************************************************************************************************/
/*  SCHEDULER SEQUENTIAL                                                                         */
/*  ====================                                                                         */
/*  This scheduler can allocate sequentially sets of threads in one or more than one cores.      */
/*  considering every thread of a set, sequential chunks of the same process.                    */
/*  Every process is allocated on a different core                                               */
/*                                                                                               */
/*  It has been developed thinking in pinballs and the way that Sniper consider every pinball as */
/*  a different application.                                                                     */
/*                                                                                               */
/*  To set-up the pinballs sets that conforms an entire process and for gather the outputs it    */
/*  was added new parameters.                                                                    */
/*    - scheduler/sequential/sequence : To indicate the sets of pinballs/threads that belongs    */
/*        to the same process.                                                                   */
/*    - scheduler/sequential/sequence_file : If it is not setted, the gathered data will be      */
/*        showed on the standard output. In the indicated file aversely.                         */
/*                                                                                               */
/*  Example: We have 3 processes and split every one in 2 pinballs.                              */
/*      ./run-sniper                                                                             */
/*         --pinballs=pb1,pb2,pb3,pb4,pb5,pb6                                                    */
/*         -g --scheduler/type=sequential                                                        */
/*         -g --scheduler/sequential/sequence="2|2|2"                                            */
/*         -g --scheduler/sequential/sequence_file="FILE.OUT"                                    */
/*         --sim-end=last                                                                        */
/*         -n 3                                                                                  */
/*************************************************************************************************/

#include "scheduler_sequential.h"
#include "config.hpp"
#include <thread.h>
#include <core_manager.h>
#include <dvfs_manager.h>
#include <trace_manager.h>

#include <unistd.h> // sleep

#include <iostream>
#include <fstream>

#include <boost/algorithm/string.hpp>

#include "performance_model.h"
#include "stats.h"


SchedulerSequential::SchedulerSequential(ThreadManager *thread_manager)
    : SchedulerPinnedBase(thread_manager, SubsecondTime::NS(10000000000)) // quantum, change to max value
    , period(Sim()->getDvfsManager()->getGlobalDomain()->getPeriod()) // not valid if f change dynamically
{
    Sim()->getHooksManager()->registerHook(HookType::HOOK_SIM_END, hook_sim_end, (UInt64)this, HooksManager::ORDER_ACTION);

    String sequence;
    std::vector<String> aux_seqs;

    sequence = Sim()->getCfg()->getString("scheduler/sequential/sequence");
    verbose = Sim()->getCfg()->hasKey("scheduler/sequential/verbose");
    outfile = Sim()->getCfg()->getString("scheduler/sequential/sequence_file");

    boost::split(aux_seqs, sequence, boost::is_any_of("|"));
    for ( long unsigned int i=0; i<aux_seqs.size(); ++i )
    {
        seqs.push_back(atoi(aux_seqs.at(i).c_str()));
    }

    LOG_ASSERT_ERROR(Sim()->getConfig()->getApplicationCores() >= seqs.size(),
            "Not enought cores. Every pinballs set must be executed on a different core.");

    core_waiting_threads.resize(Sim()->getConfig()->getApplicationCores());

    // Setting the first Thread of every core
    int next_tte = 0;
    next_thread_to_execute.resize(Sim()->getConfig()->getApplicationCores());

    for(long unsigned int i=0; i < next_thread_to_execute.size(); ++i)
    {
        next_thread_to_execute.at(i) = next_tte;
        next_tte += seqs.at(i);
    }

    current_pinball_set = 0;
    total_pinballs = 0;

    // Adding stats for cache counters
    l1d_load_miss_stat  = ThreadStatNamedStat::registerStat("l1d_load_miss", "L1-D", "load-misses");
    l1d_store_miss_stat = ThreadStatNamedStat::registerStat("l1d_store_miss", "L1-D", "store-misses");

    l1i_load_miss_stat = ThreadStatNamedStat::registerStat("l1i_load_miss", "L1-I", "load-misses");
    //l1i_store_miss_stat = ThreadStatNamedStat::registerStat("l1i_store_miss", "L1-I", "store-misses");

    l2_load_miss_stat  = ThreadStatNamedStat::registerStat("l2_load_miss", "L2", "load-misses");
    l2_store_miss_stat = ThreadStatNamedStat::registerStat("l2_store_miss", "L2", "store-misses");

    l3_load_miss_stat  = ThreadStatNamedStat::registerStat("l3_load_miss", "L3", "load-misses");
    l3_store_miss_stat = ThreadStatNamedStat::registerStat("l3_store_miss", "L3", "store-misses");

    assert(l1d_load_miss_stat != ThreadStatsManager::INVALID);
}

void SchedulerSequential::threadStart(thread_id_t thread_id, SubsecondTime time)
{
    total_pinballs++;
    core_id_t core_id = (core_id_t)atoi(
            m_thread_info[thread_id].getAffinityString().c_str() );

    if (/*m_core_thread_running[core_id] == INVALID_THREAD_ID && */
            /*thread_id == next_thread_to_execute.at(core_id)*/
            m_core_thread_running[core_id] == thread_id)
    {
        // Only the first thread of every core execute this
        next_thread_to_execute.at(core_id)++;
        // Seems that the first thread of every core has been allocated,
        // then rescheduler is not needed.
        //reschedule(time, core_id, false);
    }
    else if (m_core_thread_running[core_id] != thread_id)
    {
        aux_mm << "Pushed to core queue " << core_id;
        print_message(thread_id, aux_mm.str().c_str());
        aux_mm.str("");

        m_threads_runnable[thread_id] = false;
        core_waiting_threads[core_id].push(thread_id);
    }
}

void SchedulerSequential::threadExit(thread_id_t thread_id, SubsecondTime time)
{
    print_message(thread_id, "Finnish it's job.");
    m_threads_runnable[thread_id] = false;

    core_id_t current_core_id = (core_id_t)atoi( m_thread_info[thread_id].getAffinityString().c_str() );

    // Updating info, ended thread
    Sim()->getThreadStatsManager()->update(thread_id, time);
    m_thread_info[thread_id].setLastScheduledOut(time);

    // Alocating next thread on queue
    if ( !core_waiting_threads[current_core_id].empty() )
    {
        thread_id_t next_thread = core_waiting_threads[current_core_id].top();

        // I am not sure if the first thread can end its job while the initialization
        // of the other threads is executing and if the initialization of threads could
        // be in a disordered way respect the --pinballs option as well
        if ( next_thread != next_thread_to_execute.at(current_core_id) )
            return;

        core_waiting_threads[current_core_id].pop();
        m_threads_runnable[next_thread] = true;
        next_thread_to_execute.at(current_core_id)++;

        //Sim()->getThreadStatsManager()->update(next_thread, time);
        reschedule(time, current_core_id, false);
        print_message(next_thread, "Scheduled.");
    }
}

void SchedulerSequential::threadSetInitialAffinity ( thread_id_t thread_id )
{
    core_id_t core = (core_id_t)current_pinball_set;
    m_thread_info[thread_id].setAffinitySingle(core);
    m_thread_info[thread_id].setExplicitAffinity();

    seqs[current_pinball_set]--;
    if ( seqs[current_pinball_set] == 0 )
        current_pinball_set++;
}

void SchedulerSequential::__sim_end(SubsecondTime time)
{
    if ( outfile.length() == 0 ) results_on_screen();
    else results_on_file();
}

void SchedulerSequential::results_on_screen()
{
    core_id_t last_core = 0;
    ThreadStatsManager * tsm = Sim()->getThreadStatsManager();

    std::cout << "[REPORT] Sliced by threads" << std::endl;
    std::cout << "[REPORT][CORE:" << last_core << "] " << std::endl;
    for (long unsigned int i=0; i<total_pinballs; ++i)
    {
        core_id_t core = (core_id_t)atoi( m_thread_info[i].getAffinityString().c_str() );
        if ( core != last_core )
            std::cout << "[REPORT][CORE:" << core << "]" << std::endl;

        last_core = core;

        SubsecondTime time_ss = SubsecondTime();
        time_ss.setInternalDataForced(tsm->getThreadStatistic(i, ThreadStatsManager::ELAPSED_NONIDLE_TIME));

        UInt64 actual_cycles = SubsecondTime::divideRounded(time_ss, period);

        std::cout << "[REPORT][THREAD:" << i << "] " <<
                " CYCLES: "   << actual_cycles  <<
                " TIME:" << time_ss.getNS() <<
                " INSTR:" << tsm->getThreadStatistic(i, ThreadStatsManager::INSTRUCTIONS) <<
                " L1D-miss:" << tsm->getThreadStatistic(i, l1d_load_miss_stat) + tsm->getThreadStatistic(i, l1d_store_miss_stat) <<
                " L1I-miss:" << tsm->getThreadStatistic(i, l1i_load_miss_stat) <<
                " L2-miss:" << tsm->getThreadStatistic(i, l2_load_miss_stat) + tsm->getThreadStatistic(i, l2_store_miss_stat) <<
                " L3-miss:" << tsm->getThreadStatistic(i, l3_load_miss_stat) + tsm->getThreadStatistic(i, l3_store_miss_stat) <<
                std::endl;
    }
}

void SchedulerSequential::results_on_file()
{
    ThreadStatsManager * tsm = Sim()->getThreadStatsManager();

    std::ofstream file;
    file.open(outfile.c_str());

    for (long unsigned int i=0; i<total_pinballs; ++i)
    {
        SubsecondTime time_ss = SubsecondTime();
        time_ss.setInternalDataForced(tsm->getThreadStatistic(i, ThreadStatsManager::ELAPSED_NONIDLE_TIME));
        UInt64 actual_cycles = SubsecondTime::divideRounded(time_ss, period);

        file << time_ss.getNS() << ":" <<
                tsm->getThreadStatistic(i, ThreadStatsManager::INSTRUCTIONS) << ":" <<
                actual_cycles << ":" <<
                tsm->getThreadStatistic(i, l1d_load_miss_stat) + tsm->getThreadStatistic(i, l1d_store_miss_stat) << ":" <<
                tsm->getThreadStatistic(i, l2_load_miss_stat) + tsm->getThreadStatistic(i, l2_store_miss_stat) << ":" <<
                tsm->getThreadStatistic(i, l3_load_miss_stat) + tsm->getThreadStatistic(i, l3_store_miss_stat) << "\n";
    }

    file.close();
}

void SchedulerSequential::print_message(thread_id_t thread_id, const char * message)
{
    if ( verbose )
        std::cout << "[SCHED] " << thread_id << ": "<< message << std::endl;
}



