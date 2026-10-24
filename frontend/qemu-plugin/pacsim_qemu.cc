#include "pacsim_qemu.h"
#include "pacsim_sampler.h"

#include <cinttypes>
#include <elf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <vector>

namespace pacsim_qemu
{

namespace
{
   pacsim::Options g_options;
   bool g_enabled = false;
   std::string g_syms;
   std::set<uint64_t> g_sync_entries;
   Hooks g_hooks;

   class QemuFrontEnd : public pacsim::FrontEnd
   {
      std::mutex m_mutex;
   public:
      bool inRoi() { return g_hooks.in_roi(); }
      bool channelAlive(unsigned tid) { return g_hooks.channel_alive(tid); }
      uint64_t magic(unsigned tid, uint64_t cmd, uint64_t arg) { return g_hooks.magic(tid, cmd, arg); }
      void setDetailed() { g_hooks.set_mode(0); }
      void setFastForward(pacsim::FastForwardKind kind) { g_hooks.set_mode(kind == pacsim::FastForwardKind::Warmup ? 1 : 2); }
      void flushCounts(unsigned tid) { g_hooks.flush_counts(tid); }
      bool tryLock() { return m_mutex.try_lock(); }
      void lock() { m_mutex.lock(); }
      void unlock() { m_mutex.unlock(); }
      void log(const std::string &line) { fprintf(stderr, "%s\n", line.c_str()); }
   };

   QemuFrontEnd *g_front_end = nullptr;
   pacsim::Sampler *g_sampler = nullptr;

   std::vector<std::string> g_extra_sync;   /* pacsim-sync */

   bool is_sync_name(const char *name)
   {
      static const char *exact[] = { "pthread_barrier_wait", "GOMP_barrier", "GOMP_parallel", "__kmp_barrier",
                                     "__kmpc_barrier", "__kmp_invoke_microtask", "MPI_Barrier", "PMPI_Barrier", nullptr };
      for (const char **e = exact; *e; e++)
         if (strcmp(name, *e) == 0) return true;
      for (const std::string &e : g_extra_sync)
         if (e == name) return true;
      return strstr(name, "_omp_fn") != nullptr;
   }

   /* The synchronisation points' entry addresses, from the guest ELF's
      symbol table.  A statically linked guest has them all; a dynamically
      linked one only its own outlined OpenMP regions, since the libraries
      are mapped by the guest's loader at addresses this cannot know. */
   void load_syms(const std::string &path)
   {
      std::ifstream f(path.c_str(), std::ios::binary);
      if (!f) { fprintf(stderr, "[PacSim] cannot read %s for its symbols\n", path.c_str()); return; }
      std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (data.size() < sizeof(Elf64_Ehdr) || memcmp(data.data(), ELFMAG, SELFMAG) != 0 || data[EI_CLASS] != ELFCLASS64)
      { fprintf(stderr, "[PacSim] %s is not a 64-bit ELF\n", path.c_str()); return; }
      const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data.data();
      if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > data.size()) return;
      const Elf64_Shdr *sh = (const Elf64_Shdr *)(data.data() + eh->e_shoff);
      unsigned found = 0;
      for (unsigned i = 0; i < eh->e_shnum; i++)
      {
         if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
         if (sh[i].sh_link >= eh->e_shnum) continue;
         const Elf64_Shdr &strs = sh[sh[i].sh_link];
         if (sh[i].sh_offset + sh[i].sh_size > data.size() || strs.sh_offset + strs.sh_size > data.size()) continue;
         const Elf64_Sym *sym = (const Elf64_Sym *)(data.data() + sh[i].sh_offset);
         const char *strtab = data.data() + strs.sh_offset;
         for (uint64_t s = 0; s < sh[i].sh_size / sizeof(Elf64_Sym); s++)
         {
            if (ELF64_ST_TYPE(sym[s].st_info) != STT_FUNC || sym[s].st_value == 0 || sym[s].st_name >= strs.sh_size) continue;
            if (is_sync_name(strtab + sym[s].st_name)) { g_sync_entries.insert(sym[s].st_value); found++; }
         }
      }
      fprintf(stderr, "[PacSim] %u synchronisation points from %s\n", found, path.c_str());
   }
}

bool option(const std::string &key, const std::string &val)
{
   if (key == "pacsim")                    { g_enabled = val.empty() || (val != "0" && val != "off" && val != "false"); return true; }
   if (key == "pacsim-dir")                { g_options.output_dir = val; return true; }
   if (key == "pacsim-region-size")        { g_options.region_size = strtoull(val.c_str(), nullptr, 0) * 1000; return true; }
   if (key == "pacsim-min-region-size")    { g_options.min_region_size = strtoull(val.c_str(), nullptr, 0) * 1000; return true; }
   if (key == "pacsim-cluster-threshold")  { g_options.cluster_threshold = strtod(val.c_str(), nullptr); return true; }
   if (key == "pacsim-min-measurements")   { g_options.min_measurements = (unsigned)strtoul(val.c_str(), nullptr, 0); return true; }
   if (key == "pacsim-warmup")             { g_options.fast_forward = (val == "0" || val == "off" || val == "false") ? pacsim::FastForwardKind::Icount : pacsim::FastForwardKind::Warmup; return true; }
   if (key == "pacsim-syms")               { g_syms = val; return true; }
   if (key == "pacsim-converge")           { g_options.converge = strtod(val.c_str(), nullptr); return true; }
   if (key == "pacsim-warmup-window")      { g_options.warmup_window = strtoull(val.c_str(), nullptr, 0) * 1000; return true; }
   if (key == "pacsim-loop-min")           { g_options.loop_min = strtoull(val.c_str(), nullptr, 0); return true; }
   if (key == "pacsim-markers")            { g_options.conditional_only = val != "all"; return true; }
   if (key == "pacsim-sync")
   {
      size_t at = 0;
      while (at <= val.size())
      {
         const size_t comma = val.find(',', at);
         const std::string name = val.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
         if (!name.empty()) g_extra_sync.push_back(name);
         if (comma == std::string::npos) break;
         at = comma + 1;
      }
      return true;
   }
   return false;
}

bool enabled() { return g_enabled; }

void activate(const Hooks &hooks, unsigned max_vcpus, bool verbose)
{
   g_hooks = hooks;
   g_options.max_threads = max_vcpus;
   g_options.verbose = verbose;
   if (!g_syms.empty())
      load_syms(g_syms);
   g_front_end = new QemuFrontEnd();
   g_sampler = new pacsim::Sampler(g_options, *g_front_end);
}

void announce()
{
   fprintf(stderr, "[PacSim] online sampling enabled (regions of %" PRIu64 "K instructions, cluster threshold %g, %s between measured regions, output %s)\n",
           g_options.region_size / 1000, g_options.cluster_threshold,
           g_options.fast_forward == pacsim::FastForwardKind::Warmup ? "cache warm-up" : "instruction counting", g_options.output_dir.c_str());
}

void thread_start(unsigned cpu) { if (g_sampler) g_sampler->threadStart(cpu); }
bool block(unsigned cpu, uint32_t n, uint64_t vaddr, bool can_block) { return g_sampler->block(cpu, n, vaddr, can_block); }
bool back_edge(unsigned cpu, uint64_t vaddr, bool conditional, bool can_block) { return g_sampler->backEdge(cpu, vaddr, conditional, can_block); }
bool sync_point(unsigned cpu, uint64_t site, bool can_block) { return g_sampler->syncPoint(cpu, site, can_block); }
bool is_sync_entry(uint64_t vaddr) { return g_enabled && g_sync_entries.count(vaddr) > 0; }
void roi_begin(unsigned cpu) { if (g_sampler) g_sampler->roiBegin(cpu); }
void roi_end(unsigned cpu) { if (g_sampler) g_sampler->finish(cpu, 0, true); }
void at_exit() { if (g_sampler) g_sampler->finish(0, 0, false); }

}
