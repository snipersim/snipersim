#include "mm/proc_vma.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/mman.h>

/* Parse one line of /proc/<pid>/maps.  Format:
   start-end perms offset dev inode  pathname
   7f8a1000-7f8a2000 r-xp 00001000 08:01 12345  /lib/libc.so.6  */
static bool parse_maps_line(const char *line, ProcVma &out)
{
    uint64_t start, end, offset;
    unsigned dev_major, dev_minor;
    unsigned long inode;
    char perms[8] = {};
    char path[512] = {};

    int n = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %511[^\n]",
                   &start, &end, perms, &offset,
                   &dev_major, &dev_minor, &inode, path);
    if (n < 7) return false;  // path may be absent

    out.start  = start;
    out.end    = end;
    out.offset = offset;
    out.path   = (n >= 8) ? path : "";

    /* Trim leading whitespace from path (sscanf leaves it). */
    while (!out.path.empty() && out.path[0] == ' ')
        out.path.erase(0, 1);

    /* Convert perms string "rwxp"/"rwxs" to PROT_* + MAP_* */
    out.prot = 0;
    if (perms[0] == 'r') out.prot |= PROT_READ;
    if (perms[1] == 'w') out.prot |= PROT_WRITE;
    if (perms[2] == 'x') out.prot |= PROT_EXEC;
    out.flags = (perms[3] == 's') ? MAP_SHARED : MAP_PRIVATE;

    return true;
}

int ProcVmaTree::refresh(pid_t pid)
{
    char fname[64];
    snprintf(fname, sizeof(fname), "/proc/%d/maps", (int)pid);

    std::ifstream f(fname);
    if (!f.is_open()) {
        std::cerr << "[ProcVmaTree] cannot open " << fname << std::endl;
        return -1;
    }

    m_pid = pid;
    m_vmas.clear();

    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        ProcVma v;
        if (parse_maps_line(line.c_str(), v)) {
            m_vmas[v.start] = std::move(v);
            count++;
        }
    }
    return count;
}

const ProcVma* ProcVmaTree::find(uint64_t addr) const
{
    /* upper_bound(addr) → first with key > addr; step back. */
    auto it = m_vmas.upper_bound(addr);
    if (it == m_vmas.begin()) return nullptr;
    --it;
    if (it->second.contains(addr)) return &it->second;
    return nullptr;
}

void ProcVmaTree::dump(const std::string &path) const
{
    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "[ProcVmaTree] cannot open " << path << " for dump" << std::endl;
        return;
    }
    out << "# ProcVmaTree pid=" << m_pid << " vmas=" << m_vmas.size() << "\n";
    for (auto &kv : m_vmas) {
        const ProcVma &v = kv.second;
        char perm[5] = "----";
        if (v.prot & PROT_READ)  perm[0] = 'r';
        if (v.prot & PROT_WRITE) perm[1] = 'w';
        if (v.prot & PROT_EXEC)  perm[2] = 'x';
        perm[3] = (v.flags & MAP_SHARED) ? 's' : 'p';
        out << std::hex << v.start << "-" << v.end << std::dec
            << " " << perm
            << " size=" << v.size()
            << " off=0x" << std::hex << v.offset << std::dec;
        if (!v.path.empty()) out << " " << v.path;
        out << "\n";
    }
}
