// #include "virtuos_comm_allocator.h"
// #include "simulator.h"
// #include "core_manager.h"

// #include "virtuos_communication_protocol_memory_mapped.h"
// #include "virtuos_communication_protocol_pipes.h"
// #include "virtuos_communication_protocol_memory_mapped.h"
// #include "virtuos_communication_protocol_pipes.h"
// #include <fstream>
// #include <iostream>
// #include "thread.h"
// #include "performance_model.h"
// #include "stats.h"
// #include <unistd.h>
// #include </usr/include/semaphore.h>
// // Cfg
// #include "config.hpp"

// VirtuosCommModule::VirtuosCommModule(String name, int max_order): PhysicalMemoryAllocator(name)
// {
// 	stats.num_page_faults = 0;
// 	stats.page_fault_latency_sum = 0;
// 	registerStatsMetric("VirtuosCommModule", 0, "page_faults", &stats.num_page_faults);
// 	registerStatsMetric("VirtuosCommModule", 0, "page_fault_latency_sum", &stats.page_fault_latency_sum);

// 	memory_mapped_not_pipes = Sim()->getCfg()->getBool("perf_model/virtuos/memory_mapped_not_pipes");
// 	sem = new Semaphore(0);
// 	sem_virtuos = new Semaphore(0);

// }

// UInt64 VirtuosCommModule::allocate(UInt64 size, UInt64 address, UInt64 core_id, bool is_pagetable_allocation)
// {
// 	counter++;

// 	// std::cout << "Counter: " << counter << std::endl;

// 	// std::cout << "[comm_allocator]: Allocate" << std::endl;
// 	if (memory_mapped_not_pipes)
// 	{

// 		if (Sim()->getCoreManager() == NULL)
// 		{
// 			std::cout << "CoreManager: " << Sim()->getCoreManager() << std::endl;
// 			std::cout << "CoreManager is NULL" << std::endl;
// 			std::cout << "CoreId: " << core_id << std::endl;
// 			return 0;
// 		}

// 		if (core_id == -1)
// 		{
// 			std::cout << "core_id: " << core_id << std::endl;
// 			std::cout << "core_id = -1" << std::endl;
// 			return 0;
// 		}

// 		mm_package *mmpackage = Sim()->getCoreManager()->getCoreFromID(core_id)->getThread()->mmpackage;

// 		mmpackage->tag = mm_TAG_ALLOCATE;
// 		mmpackage->num0 = size;
// 		mmpackage->num1 = address;
// 		mmpackage->num2 = core_id;

// 		UInt64 time = Sim()->getCoreManager()->getCoreFromID(0)->getPerformanceModel()->getElapsedTime().getNS();

// 		mmpackage->flag = true;

// 		while(!mmpackage->flag2);
		
// 		mmpackage->flag2 = false;
// 		// // std::cout << "Sending allocate" << std::endl;
// 		// postAllocation(sem_virtuos);
// 		// // std::cout << "Waiting for allocation" << std::endl;
// 		// wait_for_allocation(sem);
// 		// // std::cout << "Received allocation" << std::endl;

// 		stats.page_fault_latency_sum += Sim()->getCoreManager()->getCoreFromID(0)->getPerformanceModel()->getElapsedTime().getNS() - time;
// 		std::cout << "Page fault latency: " << Sim()->getCoreManager()->getCoreFromID(0)->getPerformanceModel()->getElapsedTime().getNS() - time;
// 		//convert to cycles
		
// 		stats.num_page_faults++;

// 		return mmpackage->num0;
// 	}
// 	else
// 	{

// 		// PIPES
// 		if (Sim()->getCoreManager() == NULL)
// 		{ // happens twice
// 			std::cout << "CoreManager: " << Sim()->getCoreManager() << std::endl;
// 			std::cout << "CoreManager is NULL" << std::endl;
// 			std::cout << "CoreId: " << core_id << std::endl;
// 			return 0;
// 		}

// 		if (core_id == -1)
// 		{ // happens
// 			std::cout << "core_id: " << core_id << std::endl;
// 			std::cout << "core_id = -1" << std::endl;
// 			return 0;
// 		}

// 		if (!(Sim()->getCoreManager()->getCoreFromID(core_id)->getThread()->virtu_write.is_open()))
// 		{ // never happens
// 			std::cout << "virtu_write not open" << std::endl;
// 			return 0;
// 		}

// 		std::ofstream *writer = &(Sim()->getCoreManager()->getCoreFromID(core_id)->getThread()->virtu_write);

// 		UInt64 *array = new UInt64[3]; // TODO delete
// 		array[0] = size;
// 		array[1] = address;
// 		array[2] = core_id;

// 		pipes_package pack = pipes_package{pipes_TAG_ALLOCATE, array, 3};

// 		UInt64 time = Sim()->getCoreManager()->getCoreFromID(0)->getPerformanceModel()->getElapsedTime().getNS();
// 		std::cout << "Sending allocate" << std::endl;
// 		send(writer, pack);
// 		pipes_package ret = receive(&(Sim()->getCoreManager()->getCoreFromID(core_id)->getThread()->virtu_read));

// 		stats.page_fault_latency_sum += Sim()->getCoreManager()->getCoreFromID(0)->getPerformanceModel()->getElapsedTime().getNS() - time;

// 		std::cout << "Page fault latency: " << stats.page_fault_latency_sum << std::endl;
// 		stats.num_page_faults++;

// 		free(array);

// 		return ret.numbers[0];
// 	}
// }
// void VirtuosCommModule::deallocate(UInt64 region_begin, UInt64 coreID)
// {
// 	if (memory_mapped_not_pipes)
// 	{
// 		// MEMORY MAPPED
// 		mm_package *mmpackage = Sim()->getCoreManager()->getCoreFromID(coreID)->getThread()->mmpackage;

// 		mmpackage->tag = mm_TAG_DEALLOCATE;
// 		mmpackage->num0 = region_begin;
// 		mmpackage->num1 = coreID;

// 		sem_post(&mmpackage->sem1);
// 	}
// 	else
// 	{
// 		// PIPES
// 		UInt64 array[1] = {region_begin};
// 		pipes_package pack = pipes_package{pipes_TAG_DEALLOCATE, array, 1};

// 		send(&(Sim()->getCoreManager()->getCoreFromID(coreID)->getThread()->virtu_write), pack);
// 	}
// }
// std::vector<Range> VirtuosCommModule::allocate_eager_paging(UInt64 bytes, UInt64 coreID)
// {
// 	// this is not implemented!
// 	std::cout << "[comm_allocator]: allocate_eager_paging not implemented" << std::endl;
// 	return std::vector<Range>();
// }
// void VirtuosCommModule::perform_init_random(double target_fragmentation, double target_memory_percent, bool store_in_file, UInt64 coreID)
// {
// 	if (memory_mapped_not_pipes)
// 	{
// 		// MEMORY MAPPED
// 		if (Sim()->getCoreManager() == NULL)
// 		{ // happens twice
// 			std::cout << "CoreManager: " << Sim()->getCoreManager() << std::endl;
// 			std::cout << "CoreManager is NULL" << std::endl;
// 			std::cout << "CoreId: " << coreID << std::endl;
// 			return;
// 		}

// 		if (coreID == -1)
// 		{ // happens
// 			std::cout << "core_id: " << coreID << std::endl;
// 			std::cout << "core_id = -1" << std::endl;
// 			return;
// 		}

// 		mm_package *mmpackage = Sim()->getCoreManager()->getCoreFromID(coreID)->getThread()->mmpackage;

// 		mmpackage->tag = mm_TAG_PERFORM_INIT_RANDOM;

// 		union
// 		{
// 			double d;
// 			UInt64 i;
// 			bool b;
// 		} u;

// 		u.d = target_fragmentation;
// 		mmpackage->num0 = u.i;
// 		u.d = target_memory_percent;
// 		mmpackage->num1 = u.i;
// 		u.b = store_in_file;
// 		mmpackage->num2 = u.i;
// 		std::cout << "Performing init random" << std::endl;
// 		// sem_post(&mmpackage->sem1);
// 	}
// 	else
// 	{

// 		// PIPES
// 		if (Sim()->getCoreManager() == NULL)
// 		{
// 			std::cout << "CoreManager: " << Sim()->getCoreManager() << std::endl;
// 			std::cout << "CoreManager is NULL" << std::endl;
// 			return;
// 		}

// 		if (coreID == -1)
// 		{ // happens
// 			std::cout << "core_id: " << coreID << std::endl;
// 			std::cout << "core_id = -1" << std::endl;
// 			return;
// 		}

// 		if (!(Sim()->getCoreManager()->getCoreFromID(coreID)->getThread()->virtu_write.is_open()))
// 		{
// 			std::cout << "virtu_write not open" << std::endl;
// 			return;
// 		}

// 		union
// 		{
// 			double d;
// 			UInt64 i;
// 			bool b;
// 		} u;
// 		u.d = target_fragmentation;
// 		UInt64 target_fragmentation_int = u.i;
// 		u.d = target_memory_percent;
// 		UInt64 target_memory_percent_int = u.i;
// 		u.b = store_in_file;
// 		UInt64 store_in_file_int = u.i;

// 		UInt64 array[3] = {target_fragmentation_int, target_memory_percent_int, store_in_file_int};
// 		pipes_package pack = pipes_package{pipes_TAG_PERFORM_INIT_RANDOM, array, 3};
// 		// send(&(Sim()->getCoreManager()->getCoreFromID(coreID)->getThread()->virtu_write), pack);
// 	}
// }
// void VirtuosCommModule::print_allocator(UInt64 core_id)
// {
// 	if (memory_mapped_not_pipes)
// 	{
// 		// MEMORY MAPPED
// 		mm_package *mmpackage = Sim()->getCoreManager()->getCoreFromID(core_id)->getThread()->mmpackage;

// 		mmpackage->tag = mm_TAG_PRINT_ALLOCATOR;

// 		sem_post(&mmpackage->sem1);
// 	}
// 	else
// 	{
// 		// PIPES
// 		pipes_package pack = pipes_package{pipes_TAG_PRINT_ALLOCATOR, new UInt64[0]{}, 0};
// 		send(&(Sim()->getCoreManager()->getCoreFromID(core_id)->getThread()->virtu_write), pack);
// 	}
// }
