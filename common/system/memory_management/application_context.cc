#include "application_context.h"
#include "pagetable_factory.h"
#include "rangetable_factory.h"
#include "debug_config.h"
#include "simulator.h"
#include "config.hpp"
#include <iterator>

ApplicationContext::ApplicationContext(int app_id,
                                        const String& page_table_type,
                                        const String& page_table_name,
                                        const String& range_table_type,
                                        const String& range_table_name,
                                        bool is_guest)
    : m_app_id(app_id)
    , m_is_guest(is_guest)
    , m_page_table(nullptr)
    , m_range_table(nullptr)
{
    // Create page table using factory
    m_page_table = ParametricDramDirectoryMSI::PageTableFactory::createPageTable(
        page_table_type, page_table_name, app_id, is_guest);
    
    // Create range table using factory
    m_range_table = ParametricDramDirectoryMSI::RangeTableFactory::createRangeTable(
        range_table_type, range_table_name, app_id);
    
#if DEBUG_MIMICOS >= DEBUG_BASIC
    std::cout << "[ApplicationContext] Created context for app " << app_id 
              << " (guest=" << is_guest << ")" << std::endl;
#endif
}

ApplicationContext::~ApplicationContext()
{
    // Note: page_table and range_table are created by factories
    // and may be owned elsewhere. Check ownership semantics.
    // For now, we assume MimicOS/factories manage their lifetime.
#if DEBUG_MIMICOS >= DEBUG_BASIC
    std::cout << "[ApplicationContext] Destroying context for app " << m_app_id << std::endl;
#endif
}

VMA* ApplicationContext::findVMA(IntPtr address)
{
    for (auto& vma : m_vmas) {
        if (vma.contains(address)) {
            return &vma;
        }
    }
    return nullptr;
}

const VMA* ApplicationContext::findVMA(IntPtr address) const
{
    for (const auto& vma : m_vmas) {
        if (vma.contains(address)) {
            return &vma;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// VMA source resolution – tries multiple paths in priority order
// ---------------------------------------------------------------------------

bool ApplicationContext::parseVMAsFromFile(const std::string& trace_file_path)
{
    // 1. Try <trace>.vma (simple hex-range format – fastest)
    std::string plain_path = trace_file_path + ".vma";
    if (parseVMAsFromPlainFile(plain_path)) {
        return true;
    }

    // 1b. The recorder's own name for it.  `record-trace --vma -o X` writes
    //     X.sift and X.vma, and the trace is named here with its .sift, so
    //     the file is <trace minus .sift>.vma rather than <trace>.vma.
    std::string recorder_path;
    {
        const std::string sift = ".sift";
        if (trace_file_path.size() > sift.size() &&
            trace_file_path.compare(trace_file_path.size() - sift.size(), sift.size(), sift) == 0) {
            recorder_path = trace_file_path.substr(0, trace_file_path.size() - sift.size()) + ".vma";
            if (parseVMAsFromPlainFile(recorder_path)) {
                return true;
            }
        }
    }

    // 2. Try <trace>.vma.json (vma_infer JSON sitting next to the trace)
    std::string json_beside = trace_file_path + ".vma.json";
    if (parseVMAsFromJSON(json_beside)) {
        return true;
    }

    // 3. Try a centralised VMA JSON directory from config:
    //    perf_model/mimicos_host/vma_json_dir = "/some/path"
    //    File: <vma_json_dir>/<trace_stem>.vma.json
    try {
        if (Sim()->getCfg()->hasKey("perf_model/mimicos_host/vma_json_dir")) {
            String vma_dir = Sim()->getCfg()->getString("perf_model/mimicos_host/vma_json_dir");
            if (vma_dir.size() > 0) {
                std::string stem = traceNameStem(trace_file_path);
                std::string central_path = std::string(vma_dir.c_str()) + "/" + stem + ".vma.json";
                if (parseVMAsFromJSON(central_path)) {
                    return true;
                }
            }
        }
    } catch (...) {
        // Config key missing or malformed – fall through
    }

    std::cerr << "[ApplicationContext] WARNING: No VMA source found for trace: "
              << trace_file_path << std::endl;
    std::cerr << "[ApplicationContext]   Tried: " << plain_path << std::endl;
    if (!recorder_path.empty())
        std::cerr << "[ApplicationContext]   Tried: " << recorder_path << std::endl;
    std::cerr << "[ApplicationContext]   Tried: " << json_beside << std::endl;
    std::cerr << "[ApplicationContext]   Hint:  Run `python -m vma_infer --trace <trace> --out <trace>.vma.json`" << std::endl;
    return false;
}

// ---------------------------------------------------------------------------
// Simple hex-range .vma parser  (existing format: one "START-END" per line)
// ---------------------------------------------------------------------------

bool ApplicationContext::parseVMAsFromPlainFile(const std::string& path)
{
    std::ifstream trace(path.c_str());
    if (!trace.is_open()) {
        return false;   // silent – caller will try next source
    }

    std::cout << "[ApplicationContext] Parsing VMAs (plain) from: " << path << std::endl;

    m_vmas.clear();
    std::string line;

    while (std::getline(trace, line)) {
        std::stringstream ss(line);
        std::string startStr, endStr;

        if (std::getline(ss, startStr, '-') && std::getline(ss, endStr)) {
            try {
                IntPtr start = std::stoull(startStr, nullptr, 16);
                IntPtr end   = std::stoull(endStr, nullptr, 16);
#if DEBUG_MIMICOS >= DEBUG_DETAILED
                std::cout << "[ApplicationContext] VMA: 0x" << std::hex << start
                          << " - 0x" << end << std::dec << std::endl;
#endif
                m_vmas.emplace_back(start, end);
            }
            catch (const std::invalid_argument&) {
                std::cerr << "[ApplicationContext] Invalid VMA format: " << line << std::endl;
            }
            catch (const std::out_of_range&) {
                std::cerr << "[ApplicationContext] VMA out of range: " << line << std::endl;
            }
        }
    }

    std::cout << "[ApplicationContext] Parsed " << m_vmas.size() << " VMAs (plain) for app " << m_app_id << std::endl;
    return !m_vmas.empty();
}

// ---------------------------------------------------------------------------
// JSON parser for vma_infer output
//
// Expected structure:
// {
//   "page_size": 4096,
//   "num_regions": N,
//   "regions": [
//     { "start_addr_int": <uint64>, "end_addr_int": <uint64>, ... },
//     ...
//   ]
// }
//
// We also accept the hex-string fallback:
//   "start_addr": "0x...", "end_addr": "0x..."
// ---------------------------------------------------------------------------

namespace {

// Tiny helpers – avoid pulling in a full JSON library for two integer fields.

static int64_t json_extract_int(const std::string& blob, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = blob.find(needle);
    if (pos == std::string::npos) return -1;
    pos = blob.find(':', pos + needle.size());
    if (pos == std::string::npos) return -1;
    ++pos;
    while (pos < blob.size() && (blob[pos] == ' ' || blob[pos] == '\t')) ++pos;
    bool negative = false;
    if (pos < blob.size() && blob[pos] == '-') { negative = true; ++pos; }
    int64_t val = 0;
    while (pos < blob.size() && blob[pos] >= '0' && blob[pos] <= '9') {
        val = val * 10 + (blob[pos] - '0');
        ++pos;
    }
    return negative ? -val : val;
}

static std::string json_extract_str(const std::string& blob, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = blob.find(needle);
    if (pos == std::string::npos) return "";
    pos = blob.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = blob.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    ++pos;
    auto end_pos = blob.find('"', pos);
    if (end_pos == std::string::npos) return "";
    return blob.substr(pos, end_pos - pos);
}

} // anonymous namespace

bool ApplicationContext::parseVMAsFromJSON(const std::string& path)
{
    std::ifstream f(path.c_str());
    if (!f.is_open()) {
        return false;   // silent – caller will try next source
    }

    // Slurp entire file
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    f.close();

    // Locate "regions" array
    auto regions_pos = json.find("\"regions\"");
    if (regions_pos == std::string::npos) {
        std::cerr << "[ApplicationContext] JSON has no 'regions' array: " << path << std::endl;
        return false;
    }
    auto arr_start = json.find('[', regions_pos);
    auto arr_end   = json.rfind(']');
    if (arr_start == std::string::npos || arr_end == std::string::npos || arr_end <= arr_start) {
        std::cerr << "[ApplicationContext] Malformed 'regions' array in: " << path << std::endl;
        return false;
    }

    std::cout << "[ApplicationContext] Parsing VMAs (JSON) from: " << path << std::endl;

    m_vmas.clear();
    size_t pos = arr_start;

    while (pos < arr_end) {
        auto obj_start = json.find('{', pos);
        if (obj_start == std::string::npos || obj_start >= arr_end) break;

        // Find matching closing brace
        int depth = 0;
        size_t obj_end = obj_start;
        for (; obj_end < json.size(); ++obj_end) {
            if (json[obj_end] == '{') depth++;
            if (json[obj_end] == '}') { depth--; if (depth == 0) break; }
        }
        if (depth != 0) break;

        std::string obj = json.substr(obj_start, obj_end - obj_start + 1);

        // Prefer int fields; fall back to hex-string fields
        int64_t start_int = json_extract_int(obj, "start_addr_int");
        int64_t end_int   = json_extract_int(obj, "end_addr_int");

        IntPtr start_addr = 0, end_addr = 0;

        if (start_int > 0 && end_int > 0) {
            start_addr = static_cast<IntPtr>(start_int);
            end_addr   = static_cast<IntPtr>(end_int);
        } else {
            // Try hex-string fields: "start_addr": "0x..."
            std::string s_str = json_extract_str(obj, "start_addr");
            std::string e_str = json_extract_str(obj, "end_addr");
            if (s_str.empty() || e_str.empty()) {
                pos = obj_end + 1;
                continue;   // skip malformed entry
            }
            try {
                start_addr = std::stoull(s_str, nullptr, 16);
                end_addr   = std::stoull(e_str, nullptr, 16);
            } catch (...) {
                pos = obj_end + 1;
                continue;
            }
        }

        if (end_addr > start_addr) {
#if DEBUG_MIMICOS >= DEBUG_DETAILED
            std::cout << "[ApplicationContext] VMA (JSON): 0x" << std::hex
                      << start_addr << " - 0x" << end_addr << std::dec << std::endl;
#endif
            m_vmas.emplace_back(start_addr, end_addr);
        }

        pos = obj_end + 1;
    }

    std::cout << "[ApplicationContext] Parsed " << m_vmas.size()
              << " VMAs (JSON) for app " << m_app_id << std::endl;
    return !m_vmas.empty();
}

// ---------------------------------------------------------------------------
// Utility: extract trace name stem from a full path
//   "/mnt/.../bravo.a/bravo.a_0000.champsim.gz" → "bravo.a_0000"
//   "/mnt/.../rnd.sift"                         → "rnd"
// ---------------------------------------------------------------------------

std::string ApplicationContext::traceNameStem(const std::string& trace_path)
{
    // Strip directory
    std::string name = trace_path;
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);

    // Strip known extensions (order matters – longest first)
    const char* exts[] = {
        ".champsim.gz", ".champsim", ".sift.gz", ".sift",
        ".gz", ".xz", ".zst", nullptr
    };
    for (int i = 0; exts[i]; ++i) {
        std::string ext(exts[i]);
        if (name.size() > ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            name = name.substr(0, name.size() - ext.size());
            break;
        }
    }
    return name;
}

void ApplicationContext::setVMAAllocated(size_t vma_index)
{
    if (vma_index < m_vmas.size()) {
        m_vmas[vma_index].setAllocated(true);
    }
}

void ApplicationContext::setVMAPhysicalOffset(size_t vma_index, IntPtr offset)
{
    if (vma_index < m_vmas.size()) {
        m_vmas[vma_index].setPhysicalOffset(offset);
    }
}

IntPtr ApplicationContext::getPhysicalOffset(IntPtr va) const
{
    const VMA* vma = findVMA(va);
    if (vma) {
        return vma->getPhysicalOffset();
    }
    return static_cast<IntPtr>(-1);
}

bool ApplicationContext::incrementOffsetAllocations(IntPtr va)
{
    VMA* vma = findVMA(va);
    if (vma) {
        vma->incrementSuccessfulOffsetBasedAllocations();
        return true;
    }
    return false;
}

int ApplicationContext::getOffsetAllocations(IntPtr va) const
{
    const VMA* vma = findVMA(va);
    if (vma) {
        return vma->getSuccessfulOffsetBasedAllocations();
    }
    return -1;
}

void ApplicationContext::deletePageTableEntry(IntPtr address)
{
    if (m_page_table) {
        m_page_table->deletePage(address);
    }
}

UInt64 ApplicationContext::getAccessesPerVPN(IntPtr vpn) const
{
    if (m_page_table) {
        return m_page_table->getAccessesPerVPN(vpn);
    }
    return 0;
}
