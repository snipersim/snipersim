#include "pagetable_radix.h"
#include "pagetable_fault.h"
#include "pagetable.h"
#include "simulator.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "mimicos.h"

#include "debug_config.h"
#include "sim_log.h"

#include "misc/exception_handler_base.h"
#include "sniper_space_exception_handler.h"
#include "user_space_exception_handler.h"

namespace ParametricDramDirectoryMSI
{

        /**
         * @brief Constructor for the PageTableRadix class.
         *
         * @param core_id The core ID.
         * @param name The name of the page table.
         * @param page_sizes The number of page sizes.
         * @param page_size_list The list of page sizes.
         * @param levels The number of levels in the page table.
         * @param frame_size The size of each frame.
         * @param _pwc Pointer to the Page Walk Cache (PWC).
         * @param is_guest Boolean indicating if this is a guest page table.
         */

        PageTableRadix::PageTableRadix(int core_id, String name, String type, int page_sizes, int *page_size_list, int levels, int frame_size, bool is_guest)
                : PageTable(core_id, name, type, page_sizes, page_size_list, is_guest),
                  m_frame_size(frame_size),
                  levels(levels)
        {
                // Initialize logging with SimLog
                std::string log_name = std::string(name.c_str()) + "_radix";
                m_log = new SimLog(log_name, core_id, DEBUG_PAGE_TABLE_RADIX);

                m_log->log("Creating Radix-based page table");
                bzero(&stats, sizeof(stats));
                m_log->log("After zeroing out stats");

                registerStatsMetric(name, core_id, "page_faults", &stats.page_faults);
                registerStatsMetric(name, core_id, "prefetch_page_faults", &stats.prefetch_page_faults);
                registerStatsMetric(name, core_id, "page_table_walks", &stats.page_table_walks);
                registerStatsMetric(name, core_id, "ptw_num_cache_accesses", &stats.ptw_num_cache_accesses);
                registerStatsMetric(name, core_id, "pf_num_cache_accesses", &stats.pf_num_cache_accesses);
                registerStatsMetric(name, core_id, "allocated_frames", &stats.allocated_frames);

                m_log->log("Virtual address space: ", 12 + 9 * levels, " bits (", levels, " levels of 512 entries over 4 KiB pages)");

                m_log->log("After registering stats");

                stats.page_size_discovery = new UInt64[m_page_sizes];

                for (int i = 0; i < m_page_sizes; i++)
                {
                        stats.page_size_discovery[i] = 0;
                        registerStatsMetric(name, core_id, "page_size_discovery_" + itostr(i), &stats.page_size_discovery[i]);
                }

                m_log->log("After registering stats: page_size_discovery");

                root = (PTFrame *)malloc(sizeof(PTFrame));
                root->entries = (PTEntry *)malloc(sizeof(PTEntry) * m_frame_size);

                m_log->log("After malloc of root & root->entries");

                // @hsongara: Get the OS object
                MimicOS* os;
                if (is_guest)
                        os = Sim()->getMimicOS_VM();
                else
                        os = Sim()->getMimicOS();

                m_log->log("After init os = ", (void*)os);

                m_log->log("Before root->emulated_ppn");
                // TODO revert, but this will not work if there's only a single allocator in the sys, on VirtuOS side
                root->emulated_ppn = 0; // os->getMemoryAllocator()->handle_page_table_allocations(4096);
                m_log->log("After root->emulated_ppn = ", root->emulated_ppn);
                m_log->detailed("Root frame: ", (void*)root);

                for (int i = 0; i < m_frame_size; i++)
                {
                        root->entries[i].is_pte = false;
                        root->entries[i].data.next_level = NULL;
                }
        }

        /**
         * @brief Destructor for the PageTableRadix class.
         * Cleans up allocated resources including the logging instance.
         */
        PageTableRadix::~PageTableRadix()
        {
                if (m_log)
                {
                        delete m_log;
                        m_log = nullptr;
                }
                // Note: page_size_discovery array cleanup is handled by base class
        }

        /**
         * @brief Initializes a page table walk for a given address.
         *
         * @param address The virtual address to walk.
         * @param count Boolean indicating if the walk should be counted in the statistics.
         * @param is_prefetch Boolean indicating if this is a prefetch operation.
         * @return PTWResult The result of the page table walk.
         */

        /* The index into the table at a level: level 1 is the leaf table,
           indexed by bits 12..20, level L by the nine bits above level L-1's;
           the root is level `levels`.  Four levels give the 48-bit space of
           x86-64 and of AArch64 with a 4 KiB granule, three the 39-bit space
           of RISC-V Sv39.

           Bits 48 and up are not part of the virtual address: the trace
           thread puts the application's (or core's) id there to keep the
           applications of one simulation apart (TraceThread::va2pa,
           pa_core_shift), and the page table is the application's own, so
           they are masked off.  Within the 48 bits, an address above the
           configured space (a RISC-V guest recorded in QEMU's default 48-bit
           layout and simulated against Sv39) would index the root with its
           low bits and silently alias another region, so it is refused
           instead, naming the fix. */
        IntPtr PageTableRadix::levelIndex(IntPtr address, int level)
        {
                const IntPtr va = address & ((IntPtr(1) << 48) - 1);
                const int va_bits = 12 + 9 * levels;
                LOG_ASSERT_ERROR((va >> va_bits) == 0,
                        "Address 0x%lx does not fit the %d-bit virtual address space of page table %s (%d levels). "
                        "Record the guest within that space (run-sniper --frontend=qemu does this for RISC-V with -R, "
                        "see --qemu-va-bits) or configure a %d-level table.",
                        (unsigned long)va, va_bits, getName().c_str(), levels, levels + 1);
                return (va >> (12 + 9 * (level - 1))) & 0x1FF;
        }

        PTWResult PageTableRadix::initializeWalk(IntPtr address, bool count, bool is_prefetch, bool restart_walk_after_fault)
        {

                m_log->section("RADIX Walk");
                m_log->detailed("RADIX is coming.. with address ", SimLog::hex(address));

                if (count)
                        stats.page_table_walks++;

                bool is_pagefault = false;

                accessedAddresses visited_pts; // In cases of page faults, we replay the walk but we DO NOT RESET the visited_pts as the
                                                                           // execution time will be dominated by the page fault latency anyways

                // Time --------------------------------------------------------------------------------------------------->
                // L2 TLB Miss -> initializeWalk -> handlePageFault() -> restart_walk (label) -> return PTW Result

        restart_walk: // Get the 9 MSB of the address

                IntPtr offset = levelIndex(address, levels);

                // Start the walk from the root
                PTFrame *current_frame = root;

                IntPtr ppn_result = 0;
                IntPtr page_size_result = 0;

                int counter = 0; // Stores the depth of the pointer-chasing
                int i = 0;               // Stores the page table id

                int level = levels;
                SubsecondTime pwc_latency = SubsecondTime::Zero();

                while (level > 0)
                {
                        offset = levelIndex(address, level);

                        m_log->detailed("Accessing PT address: ", (void*)current_frame, " at level: ", level, " with offset: ", offset, "and emulated ppn: ", current_frame->emulated_ppn);
                        visited_pts.push_back(PTWAccess(i, counter, (IntPtr)(current_frame->emulated_ppn * 4096 + offset * 8), current_frame->entries[offset].is_pte && current_frame->entries[offset].data.translation.valid));

                        m_log->detailed("Pushed in visited: ", i, " ", counter, " ", (IntPtr)(current_frame->emulated_ppn * 4096 + offset * 8), " ", (current_frame->entries[offset].is_pte && current_frame->entries[offset].data.translation.valid));
                        if (current_frame->entries[offset].is_pte)
                        {

                                // The entry is not valid, we need to handle a page fault
                                if (current_frame->entries[offset].data.translation.valid == false)
                                {
                                        if (restart_walk_after_fault) {
                                                bool userspace_mimicos_enabled = Sim()->getCfg()->getBool("general/enable_userspace_mimicos");
                                                if (userspace_mimicos_enabled) {
                                                        std::cout << "[FATAL] [RADIX] VirtuOS resolved Page Fault before...\n" << 
                                                                "Now that we are replaying the instruction, no Page Fault (due to missing mapping VPN -> PPN for Data Frame) should occurr for address = " << address << std::endl;
                                                        std::cout << "[FATAL] [RADIX] exiting with status code = 1" << std::endl;
                                                        exit(1);
                                                }

                                                assert(!userspace_mimicos_enabled);
                                                
                                                // The generic FaultCtx dispatch of pagetable_fault.h, as the
                                                // hashed tables raise it: the core's handler, whatever kind.
                                                raisePageFaultFromWalk(this, address, core_id, getMaxLevel());
                                        }

                                        if (!is_prefetch) stats.page_faults++;
                                        else stats.prefetch_page_faults++;
                                        is_pagefault = true;

                                        m_log->detailed("PAGE FAULT RESOLVED for address: ", SimLog::hex(address));
                                        if (restart_walk_after_fault)
                                                goto restart_walk;
                                        else {
                                                // level = number of frames needed: (level - 1) page table frames + 1 data frame
                                                return PTWResult(page_size_result, visited_pts, ppn_result, pwc_latency, is_pagefault, level-1);
                                        }
                                }
                                // We found the entry, we can return the result

                                /* A leaf at level L maps 2^(12 + 9(L-1)) bytes: 4 KiB at
                                   level 1, 2 MiB at 2, 1 GiB at 3.  Both arrays below were
                                   indexed by level - 1 but hold one entry per configured page
                                   size (two: 4 KiB and 2 MiB), so a leaf above level 2 read
                                   past them. */
                                if (count && level - 1 < m_page_sizes)
                                        stats.page_size_discovery[level - 1]++;
                                m_log->detailed("Found translation for address: ", SimLog::hex(address), " with ppn: ", current_frame->entries[offset].data.translation.ppn, " at level: ", level, " with page size: ", 12 + 9 * (level - 1));
                                // Be careful with the return values -> always return PPN_RESULT at page size granularity
                                ppn_result = current_frame->entries[offset].data.translation.ppn;
                                LOG_ASSERT_ERROR(ppn_result != static_cast<IntPtr>(-1), "PPN is invalid");
                               
                                accesses_per_vpn[address >> 12]++; // Increment the access count for the VPN

                                page_size_result = 12 + 9 * (level - 1);   // 4 KiB at level 1, 2 MiB at level 2
                                break;
                        }
                        else
                        {
                                m_log->detailed("Moving to the next level");
                                // The entry was a pointer to the next level of the page table
                                // We need to chase the pointer -> if the next level is NULL, we need to handle a page fault
                                if (current_frame->entries[offset].data.next_level == NULL)
                                {
                                        m_log->detailed("Next level is NULL, we need to allocate a new frame");
                                        if (restart_walk_after_fault) {
                                                // TODO remove code duplication, add it to a base function
                                                bool userspace_mimicos_enabled = Sim()->getCfg()->getBool("general/enable_userspace_mimicos");
                                                if (userspace_mimicos_enabled) {
                                                        std::cout << "[FATAL] [RADIX] VirtuOS resolved Page Fault before...\n" << 
                                                                "Now that we are replaying the instruction, no Page Fault (due to missing mapping VPN -> PPN for Data Frame) should occurr for address = " << address << std::endl;
                                                        std::cout << "[FATAL] [RADIX] exiting with status code = 1" << std::endl;
                                                        exit(1);
                                                }

                                                assert(!userspace_mimicos_enabled);
                                                
                                                // The generic FaultCtx dispatch of pagetable_fault.h, as above.
                                                raisePageFaultFromWalk(this, address, core_id, getMaxLevel());
                                        }

                                        if (!is_prefetch) stats.page_faults++;
                                        else stats.prefetch_page_faults++;
                                        is_pagefault = true;

                                        m_log->detailed("Page fault resolved for address: ", SimLog::hex(address));
                                        m_log->detailed("Restarting the walk");
                                        if (restart_walk_after_fault) {
                                                goto restart_walk;
                                        }
                                        else {
                                                // level = number of frames needed: (level - 1) page table frames + 1 data frame
                                                return PTWResult(page_size_result, visited_pts, ppn_result, pwc_latency, is_pagefault, level-1);
                                        }
                                }
                                else
                                {
                                        current_frame = current_frame->entries[offset].data.next_level;
                                }
                        }

                        // Move to the next level
                        // 4->3->2->1
                        level--;
                        counter++;
                }

                m_log->detailed("Finished walk for address: ", SimLog::hex(address));
                m_log->detailed("Final physical page number: ", ppn_result);
                m_log->detailed("Final page size: ", page_size_result);

                // No page fault occurred, no frames needed
                return PTWResult(page_size_result, visited_pts, ppn_result, pwc_latency, is_pagefault, 0);
        }

        int PageTableRadix::updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames)
        {
                /* Leaves are written for 4 KiB pages (level 1) and 2 MiB pages
                   (level 2) only.  A 1 GiB page (Sv39's or AArch64's 1 GiB
                   blocks, x86's) matched neither and the walk below went on down
                   past the levels; nothing allocates one today, and a run that
                   does is refused rather than mapped wrong. */
                LOG_ASSERT_ERROR(page_size == 12 || page_size == 21,
                                 "%s: a %d-bit page cannot be mapped: the radix table models 4 KiB (12) and 2 MiB (21) pages; "
                                 "1 GiB pages (30) are not supported", name.c_str(), page_size);
             m_log->detailed("I was provided with the following frames: ");
             for (size_t i = 0; i < frames.size(); i++)
             {
                     m_log->detailed("Frame: ", frames[i]);
             }
                PTFrame *current_frame = root;
                PTFrame *previous_frame = NULL;

                IntPtr offset = levelIndex(address, levels);
                IntPtr previous_offset = static_cast<IntPtr>(-1);

                int level = levels;
                int counter = 0;

                m_log->section("Update Page Table Frames");
                m_log->detailed("Updating page table frames for address: ", SimLog::hex(address), " with ppn: ", ppn, " and page size: ", page_size);

                accessedAddresses pagefault_addresses;

                // Walk the page table to the last level and update the page table frames which are not yet allocated
                int returned_frames = 0;
                int frames_used = 0;
                int frames_allocated = 0;

                while (level > 0)
                {
                        offset = levelIndex(address, level);

                        // Move to the next level of the page table

                        m_log->detailed("Accessing: ", (void*)current_frame, " at level: ", level, " with offset: ", offset);

                        if (current_frame == NULL)
                        {
                                m_log->detailed("Current frame is NULL, we need to allocate a new frame");

                                // Bounds check before accessing frames vector
                                LOG_ASSERT_ERROR(frames_used < static_cast<int>(frames.size()), 
                                        "Out of bounds access to frames vector: frames_used (%d) >= frames.size() (%zu)", 
                                        frames_used, frames.size());

                                PTFrame *new_pt_frame = new PTFrame;
                                stats.allocated_frames++;

                                new_pt_frame->entries = new PTEntry[m_frame_size];
                                m_log->detailed("Frames used so far: ", frames_used);
                                m_log->detailed("Allocating new page table frame at : ", frames[frames_used]);
                                new_pt_frame->emulated_ppn = frames[frames_used];
                                frames_used++;  // Increment after using the frame
                                frames_allocated++;
                                
                                current_frame = new_pt_frame;

                                m_log->detailed("New frame allocated: ", (void*)current_frame, " with emulated ppn: ", new_pt_frame->emulated_ppn);
                                m_log->detailed("Previous frame: ", (void*)previous_frame, " at offset ", previous_offset, " is updated with the new frame: ", (void*)current_frame);
                                previous_frame->entries[previous_offset].data.next_level = current_frame;
                                

                                bool is_pte = level == 1 ? true : false;
                                for (int i = 0; i < m_frame_size; i++)
                                {

                                        new_pt_frame->entries[i].is_pte = is_pte;
                                        new_pt_frame->entries[i].data.next_level = NULL;
                                }
                        }
                        else
                        {

                                // Allocate a new page table
                                if (page_size == 21 && level == 2)
                                {
                                        m_log->detailed("[2MiB] Let's update the PTE: ", (void*)current_frame, " with vpn = ", (address >> 12), " and ppn: ", ppn, " at level: ", level, " with page size: ", page_size);
                                        current_frame->entries[offset].data.translation.valid = true;
                                        current_frame->entries[offset].data.translation.ppn = ppn;
                                        current_frame->entries[offset].is_pte = true;
                                        accesses_per_vpn[address >> 12] = 1; // Set the access count for the VPN to 1

                                        break;
                                }
                                else if (page_size == 12 && level == 1)
                                {

                                        m_log->detailed("[4KiB] Let's update the PTE: ", (void*)current_frame, " with vpn = ", (address >> 12), " and ppn: ", ppn, " at level: ", level, " with page size: ", page_size);
                                        current_frame->entries[offset].data.translation.valid = true;
                                        current_frame->entries[offset].data.translation.ppn = ppn;
                                        current_frame->entries[offset].is_pte = true;
                                        accesses_per_vpn[address >> 12] = 1; // Set the access count for the VPN to 1

                                        break;
                                }

                                previous_frame = current_frame;
                                current_frame = current_frame->entries[offset].data.next_level;
                                m_log->detailed("Let's jump to the next level: ", (void*)current_frame);
                                previous_offset = offset;
                                level--;
                                counter++;
                                // Note: frames_used is NOT incremented here - we only increment when we actually allocate a new frame
                                returned_frames++;
                                
                        }
                }
                return frames_allocated;
        }

        void PageTableRadix::deletePage(IntPtr address)
        {

                m_log->detailed("Deleting page that corresponds to address: ", SimLog::hex(address));
                PTFrame *current_frame = root;
                IntPtr offset = levelIndex(address, levels);

                int level = levels;
                int counter = 0;

                while (level > 0)
                {
                        offset = levelIndex(address, level);

                        if (current_frame->entries[offset].is_pte)
                        {
                                m_log->detailed("Found the PTE for address: ", SimLog::hex(address), " at level: ", level, " with offset: ", offset);
                                current_frame->entries[offset].data.translation.valid = false;
                                current_frame->entries[offset].data.translation.ppn = -1;
                                break;
                        }
                        else
                        {
                                // Move to the next level of the page table
                                current_frame = current_frame->entries[offset].data.next_level;
                        }
                        level--;
                        counter++;
                }
        }

        /**
         * @brief Allocates physical space for the page table.
         *
         * @param size The size of the physical space to allocate.
         * @return IntPtr The physical address of the allocated space.
         */
        IntPtr PageTableRadix::getPhysicalSpace(int size)
        {
                // We directly ask it from the VirtuOS
                MimicOS* os;
                if (is_guest)
                        os = Sim()->getMimicOS_VM();
                else
                        os = Sim()->getMimicOS();

                return os->getMemoryAllocator()->handle_page_table_allocations(size);
        }
    }
