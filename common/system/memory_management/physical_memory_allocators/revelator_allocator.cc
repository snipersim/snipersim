// #include "revelator_allocator.h"
// #include "physical_memory_allocator.h"
// #include <utility>
// #include <city.h>
// #include <cmath>
// #include <iostream>
// #include <fstream>
// #include <list>
// #include <vector>
// #include "config.hpp"
// #include "simulator.h"
// #include <algorithm>
// #include <cmath>
// #include <random>
// #include<fixed_types.h>
// #include <tuple>
// #include "stats.h"
// #include "core_manager.h"
// #include "parametric_dram_directory_msi/memory_manager.h"

// using namespace std;

// //#define DEBUG_REVELATOR

// RevelatorAllocator::RevelatorAllocator(String name, int max_order, int kernel_size, string frag_type, int number_of_hashes): PhysicalMemoryAllocator(name),
// 																						   m_max_order(max_order),
// 																						   m_kernel_size(kernel_size)
// {
// 	this->number_of_hashes = number_of_hashes;
// 	std::cout << "------ [VirtuOS:RevelatorAllocator] Initializing RevelatorAllocator allocator ------" << std::endl;
// 	std::cout << std::endl;
// 	std::cout << "[VirtuOS:RevelatorAllocator] Memory Size: " << m_memory_size << std::endl;

// 	this->m_max_order = max_order;


// 	this->m_total_pages = m_memory_size * 1024 / 4 - m_kernel_size * 1024 / 4;




	
// 	for(int j = 0; j < this->m_total_pages; j++){
// 		this->memory_allocations.push_back(std::make_tuple(-1, -1));
// 	}
             


// 	std::cout << "[VirtuOS:RevelatorAllocator] 4KB pages in memory: " << this->m_total_pages << std::endl;
// }

// void RevelatorAllocator::init(){
// 	std::cout << "------ [VirtuOS:RevelatorAllocator] Correct the init(). ------" << std::endl;
// 	std::cout << std::endl;
// 	std::cout << "[VirtuOS:RevelatorAllocator] Memory Size: " << m_memory_size << std::endl;

// 	//////Rework
// 	UInt64 current_order = m_max_order;

// 	UInt64 total_mem_in_pages = m_memory_size * 1024 / 4 - m_kernel_size * 1024 / 4;

// 	m_total_pages = total_mem_in_pages;


// 	UInt64 available_mem_in_pages = total_mem_in_pages;

// 	UInt64 current_free = m_kernel_size * 1024 / 4;

// 	std::vector<std::tuple<UInt64, UInt64>> memory_allocations(total_mem_in_pages);
	
// 	for(int j = 0; j < total_mem_in_pages; j++){
// 		memory_allocations[j] = std::make_tuple(-1, -1);
// 	}
             

// #ifdef DEBUG_REVELATOR
// 	std::cout << "[VirtuOS:RevelatorAllocator] 4KB pages in memory: " << total_mem_in_pages << std::endl;
// #endif


// }

// UInt64 RevelatorAllocator::allocate(UInt64 size, IntPtr address, UInt64 core_id, bool is_pagetable_allocation){


// 	print_allocation_stats(core_id);

// 	double size_in_4kb_pages = size / 4096;
// 	UInt64 offset = address & 0xFFF;
// 	UInt64 to_hash = (is_pagetable_allocation) ? address : address >> 12;

// 	if (is_pagetable_allocation){
// #ifdef DEBUG_REVELATOR
// 		std::cout << "[VirtuOS:RevelatorAllocator] The location that the to-be-allocated page table frame will be stored: " << address << std::endl;
// 		std::cout << "[VirtuOS:RevelatorAllocator] Ideally the  page table frame should be allocated at: " << RevelatorAllocator::hashFunction(address, memory_allocations.size())+ (m_kernel_size * 1024 / 4)<< std::endl;
// #endif
// 	}
	
	
// #ifdef DEBUG_REVELATOR
// 	std::cout << "[VirtuOS:RevelatorAllocator] Number of hashes: " << number_of_hashes << std::endl;
// #endif

// 	IntPtr hash;
// 	for(int i = 1; i < (number_of_hashes+1); i++){

// #ifdef DEBUG_REVELATOR
// 		std::cout << "[VirtuOS:RevelatorAllocator] We need to hash the address: " << i*to_hash << std::endl;
// #endif
// 		hash = RevelatorAllocator::hashFunction( i*to_hash, memory_allocations.size());

// #ifdef DEBUG_REVELATOR
// 		std::cout << "[VirtuOS:RevelatorAllocator] Trying to allocate at hash: " << hash << std::endl;
// #endif

// 		bool enough_space_free = true;
// 		for(int j = 0; j < size_in_4kb_pages; j++){
// 			if(get<0>(memory_allocations[hash+j]) != -1){
// 				enough_space_free = false; 
// 				break;
// 			}
// 		}
// 		if(enough_space_free){
// 			for(int j = 0; j < size_in_4kb_pages; j++){
// 				memory_allocations[hash+j] = std::make_tuple(address, core_id);
// 			}
// 			UInt64 page_address = (hash + (m_kernel_size * 1024 / 4));
// 			if (is_pagetable_allocation){
// #ifdef DEBUG_REVELATOR
// 				std::cout << "[VirtuOS:RevelatorAllocator] Allocated PPN: " << page_address << "for address: " << address << std::endl;
// #endif
// 			}
// #ifdef DEBUG_REVELATOR
// 			std::cout << "[VirtuOS:RevelatorAllocator] Allocated PPN: " << page_address << "for VPN: " << (address >>12) << std::endl;
// #endif
	
// 			if (allocation_hash_stats.find(core_id) == allocation_hash_stats.end()){
// 				allocation_hash_stats[core_id] = std::vector<double>(number_of_hashes, 0);
// 				allocation_hash_stats[core_id][i-1] += 1;
// 			}
// 			else {
// 				allocation_hash_stats[core_id][i-1] += 1;
// 			}

// 			if (total_allocations.find(core_id) == total_allocations.end()){
// 					total_allocations[core_id] = 1;
// 			}
// 			else{
// 					total_allocations[core_id] +=1;
// 			} // Keeps track of total allocations per core and per hash function per core
// 			return page_address;
// 		}


// 	}

// 	IntPtr index = hash;

// 	for (int i = 0; i < memory_allocations.size(); i++){
// 		index =  (index + 1) % memory_allocations.size();
// 		if(get<0>(memory_allocations[i]) == -1){
// 			bool free_space_found = true;
// 			for (int j = 0; j < size_in_4kb_pages; j++){
// 				if(get<0>(memory_allocations[index+j]) != -1){
// 					free_space_found = false;
// 					break;
// 				}
// 			}
// 			if(free_space_found){
// 				for (int j = 0; j < size_in_4kb_pages; j++){
// 					memory_allocations[index+j] = std::make_tuple(address, core_id);
// 				}
// 				UInt64 page_address = (index + (m_kernel_size * 1024 / 4));
					
// 				if (total_allocations.find(core_id) == total_allocations.end()){
// 						total_allocations[core_id] = 1;
// 				}
// 				else{
// 						total_allocations[core_id] +=1;
// 				} // Keeps track of total allocations per core and per hash function per core
// 				return page_address + offset;
// 			}
// 		}
// 	}

// 	std::cout << "[VirtuOS:RevelatorAllocator] Not enough space in memory." << std::endl;
// 	exit(1);
// }

// void RevelatorAllocator::deallocate(UInt64 region_begin, UInt64 core_id){

// }


// std::vector<Range> RevelatorAllocator::allocate_eager_paging(UInt64, UInt64 core_id){
// 	return std::vector<Range>();
// }

// void RevelatorAllocator::print_allocator(UInt64 core_id){
// }

// UInt64 RevelatorAllocator::hashFunction(IntPtr address, int table_size)
// {
// 	UInt64 result = CityHash64((const char *)&address, 8) % table_size;
// 	return result;
// }



// void RevelatorAllocator::perform_init_random(double target_fragmentation, double target_memory_percent, bool store_in_file, UInt64 core_idx){

// 	UInt64 pages = (UInt64)(this->m_total_pages * target_memory_percent);
// 	// Ignore fragmentation for now - just allocate the pages
// 	for(int i = 0; i < pages; i++){
// 		IntPtr hash_number = 1000;
// 		while (true)
// 		{
// 			IntPtr hash = RevelatorAllocator::hashFunction( i*hash_number, memory_allocations.size());
// 			if(get<0>(memory_allocations[hash]) == -1){
// 				memory_allocations[hash] = std::make_tuple(-2, core_idx);
// 				break;
// 			}
// 			hash_number++;
			
// 		}

// 	}
// }


// void RevelatorAllocator::print_allocation_stats(UInt64 core_id){

// 	std::cout << "[VirtuOS:RevelatorAllocator] == Printing allocation stats for core: " << core_id << " ==" << std::endl;
// 	for (auto key : allocation_hash_stats){
// 		std::cout << "[VirtuOS:RevelatorAllocator] Core: " << key.first << std::endl;
// 		std::cout << "[VirtuOS:RevelatorAllocator] Allocation stats: " << std::endl;
// 		int counter = 0;
// 		for (auto value : key.second){
// 			std::cout << "[VirtuOS:RevelatorAllocator] Hash function[" << counter << "]: " <<  value / total_allocations[key.first]*100.0 << std::endl;
// 			counter++;
// 		}
// 	}

// }




