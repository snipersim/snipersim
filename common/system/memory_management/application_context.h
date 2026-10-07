#ifndef APPLICATION_CONTEXT_H
#define APPLICATION_CONTEXT_H

#include "pagetable.h"
#include "rangetable.h"
#include "../../../include/memory_management/misc/vma.h"
#include "fixed_types.h"
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include <iostream>

/**
 * @brief Per-application memory management context
 * 
 * This class holds all memory-related state for a single application:
 * - Page table
 * - Range table
 * - Virtual Memory Areas (VMAs)
 */
class ApplicationContext {
public:
    /**
     * @brief Construct an ApplicationContext for the given app
     * 
     * @param app_id Application ID
     * @param page_table_type Type of page table to create
     * @param page_table_name Name of the page table
     * @param range_table_type Type of range table to create
     * @param range_table_name Name of the range table
     * @param is_guest Whether this is a guest OS context
     */
    ApplicationContext(int app_id, 
                       const String& page_table_type,
                       const String& page_table_name,
                       const String& range_table_type,
                       const String& range_table_name,
                       bool is_guest);
    
    ~ApplicationContext();
    
    // Disable copy (owns resources)
    ApplicationContext(const ApplicationContext&) = delete;
    ApplicationContext& operator=(const ApplicationContext&) = delete;
    
    // Allow move
    ApplicationContext(ApplicationContext&&) = default;
    ApplicationContext& operator=(ApplicationContext&&) = default;
    
    // ============ Accessors ============
    
    int getAppId() const { return m_app_id; }
    
    ParametricDramDirectoryMSI::PageTable* getPageTable() { return m_page_table; }
    const ParametricDramDirectoryMSI::PageTable* getPageTable() const { return m_page_table; }
    
    ParametricDramDirectoryMSI::RangeTable* getRangeTable() { return m_range_table; }
    const ParametricDramDirectoryMSI::RangeTable* getRangeTable() const { return m_range_table; }
    
    std::vector<VMA>& getVMAs() { return m_vmas; }
    const std::vector<VMA>& getVMAs() const { return m_vmas; }
    
    // ============ VMA Operations ============
    
    /**
     * @brief Find the VMA containing the given address
     * 
     * @param address Virtual address to look up
     * @return Pointer to VMA if found, nullptr otherwise
     */
    VMA* findVMA(IntPtr address);
    const VMA* findVMA(IntPtr address) const;
    
    /**
     * @brief Parse VMAs from a trace file
     * 
     * Tries the following sources in order:
     *   1. <trace_file_path>.vma          (simple hex range format)
     *   1b. <trace_file_path minus .sift>.vma  (what record-trace --vma writes)
     *   2. <trace_file_path>.vma.json      (vma_infer JSON output)
     *   3. <vma_json_dir>/<trace_name>.vma.json  (centralised dir from config)
     *
     * @param trace_file_path Path to the trace file (without .vma extension)
     * @return true if parsing succeeded, false otherwise
     */
    bool parseVMAsFromFile(const std::string& trace_file_path);

private:
    /** Parse the simple hex-range .vma format. */
    bool parseVMAsFromPlainFile(const std::string& path);

    /** Parse JSON produced by vma_infer (contains "regions" array). */
    bool parseVMAsFromJSON(const std::string& path);

    /** Extract the trace name stem (e.g. "bravo.a_0000") from a full path. */
    static std::string traceNameStem(const std::string& trace_path);

public:
    
    /**
     * @brief Mark a VMA as allocated
     * 
     * @param vma_index Index of the VMA to mark
     */
    void setVMAAllocated(size_t vma_index);
    
    /**
     * @brief Set the physical offset for a VMA
     * 
     * @param vma_index Index of the VMA
     * @param offset Physical offset to set
     */
    void setVMAPhysicalOffset(size_t vma_index, IntPtr offset);
    
    /**
     * @brief Get the physical offset for a virtual address (via VMA lookup)
     * 
     * @param va Virtual address
     * @return Physical offset, or -1 if not found
     */
    IntPtr getPhysicalOffset(IntPtr va) const;
    
    /**
     * @brief Increment successful offset-based allocations for a VMA
     * 
     * @param va Virtual address to identify the VMA
     * @return true if VMA found and incremented, false otherwise
     */
    bool incrementOffsetAllocations(IntPtr va);
    
    /**
     * @brief Get successful offset-based allocations count for a VMA
     * 
     * @param va Virtual address to identify the VMA
     * @return Allocation count, or -1 if VMA not found
     */
    int getOffsetAllocations(IntPtr va) const;
    
    // ============ Page Table Operations ============
    
    /**
     * @brief Delete a page from the page table
     * 
     * @param address Virtual address of the page to delete
     */
    void deletePageTableEntry(IntPtr address);
    
    /**
     * @brief Get access count for a virtual page number
     * 
     * @param vpn Virtual page number
     * @return Number of accesses
     */
    UInt64 getAccessesPerVPN(IntPtr vpn) const;

private:
    int m_app_id;
    bool m_is_guest;
    
    // Page table (owned by MimicOS factories, we just hold pointers)
    ParametricDramDirectoryMSI::PageTable* m_page_table;
    ParametricDramDirectoryMSI::RangeTable* m_range_table;
    
    // VMAs (owned)
    std::vector<VMA> m_vmas;
};

#endif // APPLICATION_CONTEXT_H
