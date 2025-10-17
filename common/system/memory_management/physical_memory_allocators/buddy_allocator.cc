#include "buddy_allocator.h"
#include "fixed_types.h"
#include <vector>
#include <tuple>
#include <string>
#include <iostream>
#include <cmath>
#include <random>
#include <cassert>
#include <algorithm>


//#define DEBUG_BUDDY 


using namespace std;

Buddy::Buddy(int memory_size, int max_order, int kernel_size, String frag_type) :
m_memory_size(memory_size),  m_max_order(max_order), m_kernel_size(kernel_size),  m_frag_type(frag_type)

{
	log_file_name = "buddy.log";
	log_file.open(log_file_name);

#ifdef DEBUG_BUDDY
	log_file << "------ [Buddy] Initializing free lists ------" << std::endl;
	log_file << std::endl;
#endif

	for (int i = 0; i < m_max_order + 1; i++)
	{
		std::vector<std::tuple<UInt64, UInt64, bool, UInt64>> vec; // Block start, Block end
		free_list.push_back(vec);
	}
	// Set the fragmentation function based on the frag_type
	if (frag_type == "contiguity")
	{
		frag_fun = &Buddy::getAverageSizeRatio;
	}
	else if (frag_type == "largepage")
	{
		frag_fun = &Buddy::getLargePageRatio;
	}
	else
	{ // default is getLargePageRatio
		frag_fun = &Buddy::getLargePageRatio;
	}


	std::cout << "[Buddy] Memory Size: " << m_memory_size << std::endl;


	UInt64 pages_in_block = static_cast<UInt64>(pow(2, m_max_order)); // start with max

	UInt64 current_order = m_max_order;

	// Initialize the free list with all the pages in memory
	// We need to subtract the kernel size from the total memory size
	UInt64 total_mem_in_pages = m_memory_size * 1024 / 4 - m_kernel_size * 1024 / 4;

	m_total_pages = total_mem_in_pages;

	m_free_pages = m_total_pages;

	UInt64 available_mem_in_pages = total_mem_in_pages;

	UInt64 current_free = m_kernel_size * 1024 / 4;


	std::cout << "[Buddy] 4KB pages in memory: " << total_mem_in_pages << std::endl;
	std::cout << "[Buddy] 2MB pages in memory: " << total_mem_in_pages / 512 << std::endl;
	std::cout << "[Buddy] 1GB pages in memory: " << total_mem_in_pages / 512 / 512 << std::endl;


	// Initialize the free list with all the pages in memory
	while (current_free < static_cast<UInt64>(m_memory_size * 1024 / 4))
	{
		while (available_mem_in_pages >= pages_in_block)
		{
#ifdef DEBUG_BUDDY
			log_file << "[Buddy] Adding block of size " << pages_in_block << " at address " << current_free << std::endl; 
#endif
			free_list[current_order].push_back(std::make_tuple(current_free, current_free + pages_in_block - 1, false, -1));
			current_free += pages_in_block;
			available_mem_in_pages -= pages_in_block;
		}
#ifdef DEBUG_BUDDY_BUDDY_SIMPLE_THP
		log_file << "[Buddy] Order " << current_order << " has " << free_list[current_order].size() << " blocks" << std::endl;
#endif
		current_order--;
		pages_in_block = static_cast<UInt64>(pow(2, current_order));
	}
#ifdef DEBUG_BUDDY
	log_file << "[Buddy] Initialization done" << std::endl;
#endif

}

/**
 * @brief Fragment the memory to achieve a target fragmentation level.
 *
 * @param target_fragmentation The desired level of memory fragmentation.
 *
 * The function uses a random number generator with a fixed seed to ensure
 * reproducibility. It iterates through the free list of pages, starting
 * from the largest pages, and demotes them to smaller pages by splitting
 * them into smaller chunks. The process continues until the current
 * fragmentation level is less than or equal to the target fragmentation level.
 *
 * Debug information is logged if DEBUG_BUDDY is defined.
 */

void Buddy::fragmentMemory(double target_fragmentation)
{

	std::cout << "[BUDDY] Fragmenting memory to achieve target fragmentation: " << target_fragmentation << std::endl;

	std::vector<UInt64> used_pages;
	unsigned seed = 12345;
	std::mt19937 gen(seed);

	std::uniform_int_distribution<UInt64> dist(0, m_max_order - 4);


	double current_fragmentation = (this->*frag_fun)();

#ifdef DEBUG_BUDDY
	log_file << "[Artificial Fragmentation Generator] Current fragmentation: " << current_fragmentation << std::endl;
#endif

	while (current_fragmentation > target_fragmentation)
	{
		// Check if there is any large page to demote

		for (int i = m_max_order; i >= 9; i--)
		{
			if (free_list[i].size() > 0)
			{
				std::tuple<UInt64, UInt64, bool, UInt64> temp = free_list[i][0];
				free_list[i].erase(free_list[i].begin());

				UInt64 start = get<0>(temp);
				UInt64 end = get<1>(temp);
				UInt64 size = end - start + 1;
				// generate a random order between 8 and m_max_order-3, add seed
				std::uniform_int_distribution<UInt64> dist(8, i - 1);

				UInt64 random_order = dist(gen);
				UInt64 chunk = size / std::pow(2, random_order);
				UInt64 pages_in_block = pow(2, random_order);

				// Essentially, we are splitting the large page into smaller pages
				// and adding them to the free list
				// This way we are increasing the fragmentation without actually allocating memory which is very useful for testing and evaluation
				for (UInt64 j = 0; j < chunk; j++)
				{
					free_list[random_order].push_back(std::make_tuple(start + j * pages_in_block, start + (j + 1) * pages_in_block - 1, false, -1));
					assert(get<0>(free_list[random_order].back()) == (start + j * pages_in_block));
				}
				break;
			}
		}
		current_fragmentation = (this->*frag_fun)();
#ifdef DEBUG_BUDDY
		log_file << "[Artificial Fragmentation Generator] Current fragmentation: " << current_fragmentation << std::endl;
#endif
	}
	std::cout << "[BUDDY] Initialized memory with final fragmentation: " << current_fragmentation << std::endl;
	std::cout << "[BUDDY] Free pages: " << m_free_pages << std::endl;
	std::cout << "[BUDDY] Free pages in (MB): " << m_free_pages * 4 / 1024 << std::endl;
	return;
}


UInt64 Buddy::allocate(UInt64 bytes, UInt64 address, UInt64 core_id)
{

	int ind = ceil(log2(bytes / 4096));
	int i;

	for (i = ind; i <= m_max_order; i++)
	{
		if (free_list[i].size() != 0)
			break;
	}
	if (i == m_max_order + 1)
	{
#ifdef DEBUG_BUDDY
		log_file << "[Buddy] No free page inside memory" << std::endl;
#endif
		return static_cast<UInt64>(-1);
	}
	else
	{
		std::tuple<UInt64, UInt64, bool, UInt64> temp;

#ifdef DEBUG_BUDDY
		log_file << "[Buddy] Found free page in order " << i << std::endl;
#endif
		temp = free_list[i].back();
		free_list[i].pop_back();
		i--;


		// Split the block into smaller blocks until we reach the desired size

		while (i >= ind)
		{

			std::tuple<UInt64, UInt64, bool, UInt64> pair1, pair2;

			pair1 = std::make_tuple(get<0>(temp), get<0>(temp) + (get<1>(temp) - get<0>(temp)) / 2, false, -1);
			pair2 = std::make_tuple(get<0>(temp) + (get<1>(temp) - get<0>(temp)) / 2 + 1, get<1>(temp), false, -1);

			assert((get<1>(pair2) - get<0>(pair2) + 1) == pow(2, i));

			free_list[i].push_back(pair1);
			free_list[i].push_back(pair2);
			temp = free_list[i].back();
			free_list[i].pop_back();
			i--;
		}
#ifdef DEBUG_BUDDY
		log_file << "[Buddy] Allocated " << bytes << " bytes at address " << get<0>(temp) << std::endl;
#endif
		m_free_pages -= pow(2, ind);
		return get<0>(temp);
	}
	assert(false);
	return static_cast<UInt64>(-1);

}


std::pair<IntPtr,int> Buddy::allocate_contiguous(UInt64 size, UInt64 app_id)
{

	int ind = ceil(log2(size / 4096));
	int i;

	int largest_order = -1;

#ifdef DEBUG_BUDDY
	log_file << "[Buddy] Allocating " << size << " bytes for app " << app_id << std::endl;
	log_file << "[Buddy] Requested order: " << ind << std::endl;
#endif
	// Search for high order blocks first
	for (i = ind; i <= m_max_order; i++)
	{
		if (free_list[i].size() != 0)
			break;

	}

	if (i == m_max_order + 1)
	{
#ifdef DEBUG_BUDDY
		log_file << "[Buddy] I can't provide such a large contiguous memory" << std::endl;
#endif
		// Provide the largest contiguous memory available

		for (i=0; i <= m_max_order; i++)
		{
			if (free_list[i].size() >0)
			{
				largest_order = i;
			}
		}

		if (largest_order == -1)
		{
#ifdef DEBUG_BUDDY
			log_file << "[Buddy] No free page inside memory" << std::endl;
#endif
			return std::make_pair((UInt64)-1, -1);
		}
		else
		{
#ifdef DEBUG_BUDDY
			log_file << "[Buddy] Found free block in order " << largest_order << std::endl;
#endif
			std::tuple<UInt64, UInt64, bool, UInt64> temp = free_list[largest_order].back();
			free_list[largest_order].pop_back();
			m_free_pages -= pow(2, largest_order);
			return std::make_pair(get<0>(temp), get<1>(temp) - get<0>(temp) + 1);

		}

	}
	else
	{
		std::tuple<UInt64, UInt64, bool, UInt64> temp;
		std::vector<std::tuple<UInt64, UInt64, bool, UInt64>> temp_list;

#ifdef DEBUG_BUDDY
		log_file << "[Buddy] Found free block in high order " << i << " and requested order is: " << ind << std::endl;
#endif
		temp = free_list[i].back();
		free_list[i].pop_back();

		i--;

		// Split the block into smaller blocks until we reach the desired size
#ifdef DEBUG_BUDDY
		log_file << "[Buddy] Splitting block into smaller blocks" << std::endl;
#endif

		while (i >= ind)
		{

			std::tuple<UInt64, UInt64, bool, UInt64> pair1, pair2;

#ifdef DEBUG_BUDDY
			log_file << "[Buddy] Splitting block into smaller blocks of order " << i << std::endl;
			log_file << "[Buddy] Block start: " << get<0>(temp) << " Block end: " << get<1>(temp) << std::endl;
#endif

			pair1 = std::make_tuple(get<0>(temp), get<0>(temp) + (get<1>(temp) - get<0>(temp)) / 2, false, -1);
			pair2 = std::make_tuple(get<0>(temp) + (get<1>(temp) - get<0>(temp)) / 2 + 1, get<1>(temp), false, -1);

			assert((get<1>(pair2) - get<0>(pair2) + 1) == pow(2, i));

			free_list[i].push_back(pair1);
			free_list[i].push_back(pair2);
			temp = free_list[i].back();
			free_list[i].pop_back();
			i--;
		}

#ifdef DEBUG_BUDDY
		log_file << "[Buddy] Allocated " << size << " bytes at address " << get<0>(temp) << std::endl;
#endif

		m_free_pages -= pow(2, ind);

#ifdef DEBUG_BUDDY
		log_file << "[Buddy] Updated m_free_pages = " << m_free_pages << std::endl;
		log_file << "[Buddy] Returning address: " << get<0>(temp) << " and size: " << get<1>(temp) - get<0>(temp) + 1 << std::endl;
#endif

		return std::make_pair(get<0>(temp), get<1>(temp) - get<0>(temp) + 1);
		 
	}

}

std::tuple<UInt64, UInt64, bool, UInt64> Buddy::reserve_2mb_page(UInt64 address, UInt64 core_id)
{
	
	// check if there is a 2MB region available
#ifdef DEBUG_BUDDY
	log_file << "DEBUG_BUDDY: Checking for 2MB region in free_list[9]" << std::endl;
#endif

	if (free_list[9].size() > 0)
	{

#ifdef DEBUG_BUDDY
	log_file << "DEBUG_BUDDY: 2MB region available in free_list[9]" << std::endl;
#endif
		// get the 2MB region
		std::tuple<UInt64, UInt64, bool, UInt64> temp = free_list[9].back();
		free_list[9].pop_back();

#ifdef DEBUG_BUDDY
	log_file << "DEBUG_BUDDY: Retrieved and removed 2MB region from free_list[9]" << std::endl;
#endif

		return temp;
	}
	else {

#ifdef DEBUG_BUDDY
	log_file << "DEBUG_BUDDY: No 2MB region available in free_list[9], checking higher orders" << std::endl;
#endif

		for (int i = 10; i <= m_max_order; i++)
		{
			if (free_list[i].size() > 0)
			{

#ifdef DEBUG_BUDDY
			log_file << "DEBUG_BUDDY: Region available in free_list[" << i << "]" << std::endl;
#endif
				std::tuple<UInt64, UInt64, bool, UInt64> temp = free_list[i].front();
				free_list[i].erase(free_list[i].begin());
				UInt64 start = get<0>(temp);
				UInt64 end = get<1>(temp);
				UInt64 size = end - start + 1;
				UInt64 chunk = size / 512;
				UInt64 pages_in_block = pow(2, 9);

		#ifdef DEBUG_BUDDY
				log_file << "DEBUG_BUDDY: Splitting region from free_list[" << i << "] into 2MB chunks" << std::endl;
		#endif

				for (UInt64 j = 0; j < chunk; j++)
				{
					free_list[9].push_back(std::make_tuple(start + j * pages_in_block, start + (j + 1) * pages_in_block - 1, false, -1));
					assert(get<0>(free_list[9].back()) == (start + j * pages_in_block));
		#ifdef DEBUG_BUDDY
					log_file << "DEBUG_BUDDY: Added 2MB chunk to free_list[9], start = " << (start + j * pages_in_block) << std::endl;
		#endif
				}
				temp = free_list[9].back();
				free_list[9].pop_back();
		#ifdef DEBUG_BUDDY
				log_file << "DEBUG_BUDDY: Retrieved and removed 2MB region from free_list[9] after splitting" << std::endl;
				log_file << "DEBUG_BUDDY: Returning 2MB region from 4KB start page: " << get<0>(temp) << " and end page: " << get<1>(temp) << std::endl;
		#endif

				return temp;
			}
		}
	}

	#ifdef DEBUG_BUDDY
		log_file << "DEBUG_BUDDY: No region available, returning nullptr" << std::endl;
	#endif

	return std::make_tuple(-1, 0, false, 0);

}


void Buddy::free(UInt64 start, UInt64 end)
{
	int order = ceil(log2((end - start + 1)));
	int i = order;

	#ifdef DEBUG_BUDDY
		log_file << "Debug: Calculated order = " << order << std::endl;
	#endif

	std::tuple<UInt64, UInt64, bool, UInt64> temp = std::make_tuple(start, end, false, -1);

	#ifdef DEBUG_BUDDY
		log_file << "Debug: Created tuple with start = " << start << " and end = " << end << std::endl;
	#endif

	free_list[i].push_back(temp);

	#ifdef DEBUG_BUDDY
		log_file << "Debug: Pushed tuple to free_list[" << i << "]" << std::endl;
	#endif

	m_free_pages += pow(2, i);

	#ifdef DEBUG_BUDDY
		log_file << "Debug: Updated m_free_pages = " << m_free_pages << std::endl;
	#endif
}


double Buddy::getAverageSizeRatio()
{
	std::vector<UInt64> blockSizes;

	// Collect sizes of all free blocks
	for (const auto &list : free_list)
	{
		for (const auto &block : list)
		{
			// check if the block is contiguous
			blockSizes.push_back(get<1>(block) - get<0>(block) + 1);
		}
	}

	// Sort block sizes in descending order
	std::sort(blockSizes.rbegin(), blockSizes.rend());

	// Calculate average of top 50 blocks
	UInt64 totalSize = 0;
	int count = 0;
	for (auto size : blockSizes)
	{
		totalSize += size;
		count++;
		if (count == 50)
			break;
	}

	double averageSize = (count > 0) ? (double)totalSize / count : 0.0;
	return averageSize / pow(2, m_max_order - 3);
}

double Buddy::getLargePageRatio()
{
	int numberOfLargePages = 0;

	// Collect number of 2MB pages based on the free list
	for (const auto &list : free_list)
	{
		for (const auto &block : list)
		{
			if ((get<1>(block) - get<0>(block) + 1) >= 512)
			{
				numberOfLargePages += (get<1>(block) - get<0>(block) + 1) / 512;
			}
		}
	}

	// calculate the ratio of available large pages to the total number of 2MB pages
	double largePageRatio = (double)numberOfLargePages / (m_total_pages / 512);
	m_frag_factor = largePageRatio;
	return largePageRatio;
}