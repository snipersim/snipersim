#include <iostream>
#include "line_out.h"
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <memory>
#include <cstring>  // Include this to use strcpy and strncpy
#include <cassert>  // Include this to use strcpy and strncpy
#include <vector>

#include "mimicos.h"
#include "mm/allocator_factory.h"
#include "mm/fault_phases.h"
#include "mm/linux_phase_work.h"
#include "sim_api.h"
#include "globals.h"
#include "fixed_types.h"

#include "debug_config.h"


#define SIM

#define BASE_PAGE_SHIFT 12UL

#define MAX_ARGS 10
INIReader *reader;        // defined in globals.h
MetricsRegistry *m_stats; // defined in globals.h

int result_counter = 0;

MimicOS::MimicOS(std::string configurationFile, std::string outputFile, std::string appFile)
    : path_to_outputFile(outputFile), 
      path_to_app(appFile), 
      path_to_configFile(configurationFile)

{

    // Initialize configuration parser
    reader = new INIReader(configurationFile);

    // Initialize metrics registry
    m_stats = new MetricsRegistry();


    //Create physical memory allocator based on config file
    // Read configuration parameters from the INI file
    String allocatorName = reader->Get("allocator", "memory_allocator", "").c_str();

    //max_order is only applicable to Buddy-based allocators (e.g., ReserveTHP, Baseline)
    int maxOrder = reader->GetInteger("allocator", "max_order", 0);
    LineOut() << "[MimicOS]: Allocator order: " << maxOrder << std::endl;

    //by setting the kernel size, we reserve the first 'kernel_size' MB for the kernel
    int kernelSize = reader->GetInteger("pmem_alloc"    , "kernel_size", 0);
    LineOut() << "[MimicOS]: Kernel size: " << kernelSize << std::endl;

    //Fragmentation's definition depends on what we want to optimize for
    // "contiguity" => we want to optimize for contiguity (i.e., average block size)
    // "large_pages" => we want to optimize for large pages (i.e., ratio of 2MB pages)
    String fragType = reader->Get("pmem_alloc", "frag_type", "none").c_str();
        
    int memory_size = reader->GetInteger("pmem_alloc", "memory_size", 0);
    LineOut() << "[MimicOS]: Memory size: " << memory_size << std::endl;

    //threshold_for_promotion is only applicable to ReserveTHP allocator
    // As we saw in Part1.1, we do not promote a 2MB region to a full 2MB page unless
    // the fraction of used 4KB pages in that region exceeds threshold_for_promotion
    int threshold_for_promotion = reader->GetInteger("pmem_alloc", "threshold_for_promotion", -1);
    LineOut() << "[MimicOS]: Threshold for promotion (Applicable to ReserveTHP, otherwise: default = -1): " << threshold_for_promotion << std::endl;

    //Create physical memory allocator - this will be used to serve page allocations
    // throughout the execution of the SIFT-based application
    physical_memory_allocator = AllocatorFactory::createAllocator(allocatorName, memory_size, maxOrder, kernelSize, fragType, threshold_for_promotion);
    LineOut() << "[MimicOS]: Created allocator: " << allocatorName << std::endl;

    // Initialize HugeTLBfs (optional, from INI config)
    {
        bool htlbfs_enabled = reader->GetBoolean("hugetlbfs", "enabled", false);
        if (htlbfs_enabled) {
            HugeTLBfsConfig cfg;
            cfg.enabled = true;
            cfg.nr_hugepages_2mb = reader->GetInteger("hugetlbfs", "nr_hugepages_2mb", 0);
            cfg.nr_hugepages_1gb = reader->GetInteger("hugetlbfs", "nr_hugepages_1gb", 0);
            cfg.overcommit = reader->GetBoolean("hugetlbfs", "overcommit", false);
            m_hugetlbfs = std::make_unique<VirtuosoHugeTLBfs>(cfg);
            LineOut() << "[MimicOS] HugeTLBfs enabled: " << cfg.nr_hugepages_2mb << " 2MB + "
                      << cfg.nr_hugepages_1gb << " 1GB pages" << std::endl;
        }
    }

    // Initialize SwapCache (optional, from INI config)
    {
        bool swap_enabled = reader->GetBoolean("swap", "enabled", false);
        if (swap_enabled) {
            int swap_size_mb = reader->GetInteger("swap", "swap_size_mb", 1024);
            m_swap_cache = std::make_unique<VirtuosoSwapCache>(swap_size_mb);
            LineOut() << "[MimicOS] SwapCache enabled: " << swap_size_mb << " MB" << std::endl;
        }
    }

    // Initialize fault-phase instrumentation (optional, from INI config)
    {
        m_fault_calibration.phase_instrumentation_enabled =
            reader->GetBoolean("fault_calibration", "phase_instrumentation_enabled", false);
        m_fault_calibration.per_fault_logging_enabled =
            reader->GetBoolean("fault_calibration", "per_fault_logging_enabled", false);

        m_fault_calibration.vma_lookup_base_ns =
            (uint64_t)reader->GetInteger("fault_calibration", "vma_lookup_base", 0);
        m_fault_calibration.pt_page_alloc_cost_ns =
            (uint64_t)reader->GetInteger("fault_calibration", "pt_page_alloc_cost", 0);
        m_fault_calibration.phys_page_alloc_cost_ns =
            (uint64_t)reader->GetInteger("fault_calibration", "phys_page_alloc_cost", 0);
        m_fault_calibration.zero_fill_cost_ns =
            (uint64_t)reader->GetInteger("fault_calibration", "zero_fill_cost", 0);
        m_fault_calibration.pte_install_cost_ns =
            (uint64_t)reader->GetInteger("fault_calibration", "pte_install_cost", 0);
        m_fault_calibration.lock_contention_penalty_ns =
            (uint64_t)reader->GetInteger("fault_calibration", "lock_contention_penalty", 0);
        m_fault_calibration.cache_tlb_perturb_penalty_ns =
            (uint64_t)reader->GetInteger("fault_calibration", "cache_tlb_perturb_penalty", 0);
        m_fault_calibration.numa_penalty_ns =
            (uint64_t)reader->GetInteger("fault_calibration", "numa_penalty", 0);

        /* Phase 7 drift-injection knobs.  Both 0 by default (no injection). */
        m_fault_calibration.inject_drift_after_faults =
            (uint64_t)reader->GetInteger("fault_calibration", "inject_drift_after_faults", 0);
        m_fault_calibration.inject_drift_extra_iters =
            (uint64_t)reader->GetInteger("fault_calibration", "inject_drift_extra_iters", 0);
        if (m_fault_calibration.inject_drift_extra_iters > 0) {
            LineOut() << "[MimicOS] Drift injection enabled: after_faults="
                      << m_fault_calibration.inject_drift_after_faults
                      << " extra_iters=" << m_fault_calibration.inject_drift_extra_iters
                      << std::endl;
        }

        if (m_fault_calibration.phase_instrumentation_enabled) {
            m_fault_metrics.enable();
            LineOut() << "[MimicOS] Fault phase instrumentation enabled" << std::endl;
        }
    }

    // Initialize fault replay engine (optional, from INI config)
    {
        FaultReplayConfig rcfg;
        rcfg.enabled = reader->GetBoolean("fault_replay", "enabled", false);
        rcfg.force_detailed = reader->GetBoolean("fault_replay", "force_detailed", false);
        rcfg.allow_structured_replay = reader->GetBoolean("fault_replay", "allow_structured_replay", true);
        rcfg.allow_analytical_replay = reader->GetBoolean("fault_replay", "allow_analytical_replay", true);
        rcfg.warmup_count = reader->GetInteger("fault_replay", "warmup_count", 16);
        rcfg.sample_interval = reader->GetInteger("fault_replay", "sample_interval", 64);
        rcfg.stable_interval = reader->GetInteger("fault_replay", "stable_interval", 256);
        rcfg.stable_after = reader->GetInteger("fault_replay", "stable_after", 32);
        rcfg.stable_tolerance = reader->GetReal("fault_replay", "stable_tolerance", 0.05);

        m_fault_replay.configure(rcfg);
        m_fault_replay.bind_allocator(physical_memory_allocator);
    }

    // Initialize train-then-replay registry (Phase 1/2, optional).
    // Disabled by default — when enabled, collects per-fingerprint
    // cycle statistics and promotes profiles to TRAINED / ABANDONED.
    {
        if (reader->GetBoolean("fast_fault", "enabled", false)) {
            FastFaultPolicy fp;
            fp.min_samples     = reader->GetInteger("fast_fault", "min_samples", 100);
            fp.abandon_samples = reader->GetInteger("fast_fault", "abandon_samples", 500);
            fp.stable_cv       = reader->GetReal   ("fast_fault", "stable_cv", 0.10);
            fp.resample_every  = reader->GetInteger("fast_fault", "resample_every", 0);

            /* Phase 6 replay strategy knobs.  replay_mode is a string
               (full|prefix|analytical); prefix uses replay_prefix_k. */
            std::string mode_s = std::string(
                reader->Get("fast_fault", "replay_mode", "full").c_str());
            if (mode_s == "prefix" || mode_s == "PREFIX" || mode_s == "prefix_k")
                fp.replay_mode = FastFaultReplayMode::PREFIX_K;
            else if (mode_s == "analytical" || mode_s == "ANALYTICAL")
                fp.replay_mode = FastFaultReplayMode::ANALYTICAL;
            else
                fp.replay_mode = FastFaultReplayMode::FULL;
            fp.replay_prefix_k = reader->GetInteger("fast_fault", "replay_prefix_k", 16);
            fp.dict_cache      = reader->GetBoolean("fast_fault", "dict_cache", true);

            /* Phase 7 drift-detection knobs.  Off by default
               (resample_every stays 0) to preserve pre-Phase-7
               behaviour until explicitly enabled. */
            fp.drift_sigma_band      = reader->GetReal   ("fast_fault", "drift_sigma_band",       3.0);
            fp.drift_consecutive_out = reader->GetInteger("fast_fault", "drift_consecutive_out",   3);

            /* Phase 9 hierarchical-splitting knobs.  Off by default. */
            fp.hierarchical_split_enabled =
                reader->GetBoolean("fast_fault", "hierarchical_split_enabled", false);
            fp.hierarchical_split_pgtable_enabled =
                reader->GetBoolean("fast_fault", "hierarchical_split_pgtable_enabled", false);
            fp.hierarchical_split_vma_freshness_enabled =
                reader->GetBoolean("fast_fault", "hierarchical_split_vma_freshness_enabled", false);

            m_fast_fault_registry.configure(fp);
            m_fast_fault_registry.enable();
            LineOut() << "[MimicOS] FastFaultRegistry enabled (min_samples="
                      << fp.min_samples << " abandon_samples=" << fp.abandon_samples
                      << " stable_cv=" << fp.stable_cv
                      << " replay_mode=" << mode_s
                      << " replay_prefix_k=" << fp.replay_prefix_k
                      << " dict_cache=" << fp.dict_cache
                      << " split_pcp=" << fp.hierarchical_split_enabled
                      << " split_pgtable=" << fp.hierarchical_split_pgtable_enabled
                      << ")" << std::endl;
        }
    }

    // Initialize scheduler (optional, from INI config)
    {
        std::string policy_str = std::string(reader->Get("scheduler", "policy", "disabled").c_str());
        SchedulerConfig scfg;

        if (policy_str == "fifo")         scfg.policy = SchedPolicy::FIFO_SIMPLE;
        else if (policy_str == "rr")      scfg.policy = SchedPolicy::RR_SIMPLE;
        else if (policy_str == "cfs")     scfg.policy = SchedPolicy::CFS_LITE;
        else if (policy_str == "vm_aware") scfg.policy = SchedPolicy::VM_AWARE;
        else                               scfg.policy = SchedPolicy::DISABLED;

        scfg.num_cores = reader->GetInteger("scheduler", "num_cores", 1);
        scfg.default_timeslice_ns = reader->GetInteger("scheduler", "timeslice_ns", 4000000);
        scfg.min_granularity_ns = reader->GetInteger("scheduler", "min_granularity_ns", 1000000);
        scfg.yield_after_fault = reader->GetBoolean("scheduler", "yield_after_fault", true);
        scfg.yield_instruction_quantum = reader->GetInteger("scheduler", "yield_instruction_quantum", 0);
        scfg.prefer_numa_local = reader->GetBoolean("scheduler", "prefer_numa_local", true);
        scfg.avoid_fault_heavy = reader->GetBoolean("scheduler", "avoid_fault_heavy", true);

        m_scheduler.configure(scfg);
    }

    /* KernelArena: optional slab/arena for MimicOS internal metadata.
       Charges every allocation against the simulated kernel reserve.
       INI: [kernel_arena] enabled = true / size_mb = N */
    {
        bool ena = reader->GetBoolean("kernel_arena", "enabled", false);
        if (ena) {
            int mb = reader->GetInteger("kernel_arena", "size_mb", 64);
            m_kernel_arena.init((size_t)mb * 1024 * 1024,
                                physical_memory_allocator);
            /* Route Scheduler Task allocations through the arena. */
            m_scheduler.set_arena(&m_kernel_arena);
        }
    }
}

/* Stage D: trampoline for pthread_create — casts arg to PerCoreState*,
   calls kernel_worker with the core_id. */
void* MimicOS::kernel_worker_trampoline(void* arg)
{
    PerCoreState* cs = static_cast<PerCoreState*>(arg);
    MimicOS::getMimicOS()->kernel_worker(cs->core_id);
    return nullptr;
}

/* Stage D: per-core kernel worker.  Each core:
   1. Sets its thread name for Sniper diagnostics.
   2. Forks+execve's a live app (injectForkedApp targets this core's TraceThread).
   3. Calls SimThreadRoiStart for its app.
   4. Enters poll_for_signal for its core.
   The main thread calls this for core 0; spawned pthreads call it for cores 1..N-1. */
thread_local uint8_t MimicOS::m_last_pcp_class = 2;

void MimicOS::kernel_worker(int core_id)
{
    char tname[64];
    snprintf(tname, sizeof(tname), "mimicos_kernel_core%d", core_id);
    SimSetThreadName(tname);

    PerCoreState& cs = m_cores[core_id];
    const std::string& app_path = m_app_paths[core_id];
    bool is_live = (app_path.rfind("live:", 0) == 0);

    /* Stage 2B.3 (Apr 18 2026): "idle:" app type — the core has a kernel
       pthread but no initial user app.  It enters poll_for_signal directly
       and can receive a "new_thread" event when a live app on another core
       pthread_creates a worker.  This is the free core that pthread_create
       parks the child on (affinity = next free kernel). */
    bool is_idle = (app_path == "idle:" || app_path == "idle");

    if (is_live) {
        /* Live-spawn: fork+execve the binary. */
        std::string binary = app_path.substr(5);
        char id_str[16];
        snprintf(id_str, sizeof(id_str), "%d", core_id);
        fprintf(stderr, "[MimicOS] kernel_worker core %d: live app '%s' --id %s\n",
                core_id, binary.c_str(), id_str);
        cs.child_pid = spawn_live_application(binary, {"--id", id_str});
        /* Stage 3 (Apr 18 2026): learn the app_id allocated by
           injectForkedApp and seed this core's runqueue with the live
           app's main thread so the scheduler can rotate back to it on
           quantum_expired.  Without this, a preempted main thread would
           find an empty runqueue and — in our policy — just resume in
           place, which works for 1 thread on 1 core but breaks once
           workers enter the mix. */
        uint64_t my_app_id = SimGetInjectedAppId();
        if (my_app_id != (uint64_t)-1) {
            uint64_t main_atid = (my_app_id << 16) | 0;  // encode(app_id, 0)
            /* Stage 5 (Apr 19 2026): seed the scheduler with a Task for
               the live app's main thread when scheduler is enabled; fall
               back to the legacy per-core deque when policy=disabled. */
            if (m_scheduler.is_enabled()) {
                char name[64];
                snprintf(name, sizeof(name), "live_app%lu_main", (unsigned long)my_app_id);
                Task* t = m_scheduler.create_task_for_atid(name, main_atid, core_id);
                if (t) m_scheduler.wake_up(t, core_id);
            } else {
                cs.runqueue.push_back(main_atid);
            }
            fprintf(stderr, "[MimicOS] kernel_worker core %d: seeded runqueue with main_atid=0x%lx (app_id=%lu)\n",
                    core_id, (unsigned long)main_atid, (unsigned long)my_app_id);
        }
    } else if (is_idle) {
        fprintf(stderr, "[MimicOS] kernel_worker core %d: idle kernel, waiting for scheduled work\n",
                core_id);
        /* Nothing to do — poll_for_signal below will block on SimReceiveMessage.
           A scheduling event (new_thread) from another core wakes us. */
    } else {
        /* Trace-based: inject the SIFT file via SimStartProcess.
           In mixed mode, wait for a live app to enable perf model first
           (via SimThreadRoiStart barrier) so the trace doesn't race
           through all its instructions in FAST_FORWARD before detailed
           simulation starts.
           Pure-trace multi-core (no live apps): skip SimWaitForRoi —
           nobody will fire the barrier.  The trace's own SimRoiStart
           magic (embedded in the SIFT stream) will enable the perf
           model once the context-switch flips this TraceThread onto
           the app reader.  Calling SimWaitForRoi here would hang. */
        fprintf(stderr, "[MimicOS] kernel_worker core %d: trace app '%s'\n",
                core_id, app_path.c_str());
        /* Inject the trace reader FIRST (in FAST_FORWARD — safe), then
           optionally wait for perf model to enable. */
        SimStartProcess((long unsigned int)(app_path.c_str()));
        if (m_num_live_apps > 0) {
            SimWaitForRoi();  // blocks until a live app enables perf model
        }
    }

    fprintf(stderr, "[MimicOS] kernel_worker core %d: entering poll_for_signal, child=%d, is_idle=%d\n",
            core_id, (int)cs.child_pid, is_idle ? 1 : 0);
    /* Stage 2E (Apr 18 2026): only live/trace cores have an initial app to
       context-switch into at poll_for_signal entry.  Idle cores start in
       the event loop and wait for scheduling messages from other cores. */
    poll_for_signal(core_id, /*has_initial_app=*/!is_idle);

    /* Dump per-core VMA tree (live apps only). */
    if (cs.child_pid > 0 && cs.vma_tree.size() > 0) {
        std::string base = reportBase();
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "_vma_tree_core%d.log", core_id);
        cs.vma_tree.dump(base + suffix);
    }
}

MimicOS::~MimicOS()
{
    stop_daemons();

    // Dump live-app VMA tree if it was populated
    if (m_child_pid > 0 && m_app_vma_tree.size() > 0) {
        std::string base = reportBase();
        m_app_vma_tree.dump(base + "_vma_tree.log");
        LineOut() << "[MimicOS] Dumped VMA tree (" << m_app_vma_tree.size()
                  << " regions) to " << base << "_vma_tree.log" << std::endl;
    }

    // Dump fault-phase metrics if instrumentation was enabled
    if (m_fault_calibration.phase_instrumentation_enabled && m_fault_metrics.fault_count() > 0) {
        std::string base = reportBase();
        m_fault_metrics.dump_csv(base + "_fault_phases.csv");
        m_fault_metrics.dump_histograms(base + "_fault_histograms.csv");
        LineOut() << "[MimicOS] Dumped fault phase metrics (" << m_fault_metrics.fault_count()
                  << " faults) to " << base << "_fault_*.csv" << std::endl;
    }

    // Dump scheduler stats if enabled
    if (m_scheduler.is_enabled()) {
        std::string base = reportBase();
        m_scheduler.dump_stats(base + "_scheduler_stats.csv");
        auto& ss = m_scheduler.stats();
        LineOut() << "[MimicOS] Scheduler stats: csw=" << ss.context_switches
                  << " preemptions=" << ss.preemptions
                  << " wakeups=" << ss.wakeups << std::endl;
    }

    // Dump replay cache stats if enabled
    if (m_fault_replay.is_enabled()) {
        std::string base = reportBase();
        m_fault_replay.dump_stats(base + "_replay_stats.csv");
        auto& cs = m_fault_replay.cache_stats();
        LineOut() << "[MimicOS] Replay cache stats: lookups=" << cs.total_lookups()
                  << " hit_rate=" << cs.hit_rate()
                  << " structured=" << cs.structured_replays
                  << " analytical=" << cs.analytical_replays << std::endl;
    }

    // Dump fast-fault registry stats if enabled
    if (m_fast_fault_registry.is_enabled()) {
        std::string base = reportBase();
        std::ofstream f(base + "_fast_fault_profiles.csv");
        if (f.is_open()) {
            f << "fp_id,mapping,access,page_size,pcp_class,pgt_level,vma_fresh,state,samples,"
                 "mean_cycles,stddev,cv,replay_count,trace_len,"
                 "resamples,drift_in_band,drift_out_band,drift_resets,drift_last_sample,"
                 "p50,p90,p99,pcp_hit_samples,pcp_miss_samples,pcp_unknown_samples\n";
            for (const auto& kv : m_fast_fault_registry.table()) {
                const FaultFingerprint& k = kv.first;
                const FastFaultProfile& p = kv.second;
                const char* st = "unknown";
                switch (p.state) {
                    case FastFaultState::UNKNOWN:   st = "unknown"; break;
                    case FastFaultState::TRAINING:  st = "training"; break;
                    case FastFaultState::TRAINED:   st = "trained"; break;
                    case FastFaultState::ABANDONED: st = "abandoned"; break;
                }
                f << p.fp_id << ','
                  << static_cast<int>(k.mapping_type) << ','
                  << static_cast<int>(k.access_type) << ','
                  << static_cast<int>(k.target_page_size) << ','
                  << static_cast<int>(k.pcp_hit_or_miss) << ','
                  << static_cast<int>(k.pgtable_install_level) << ','
                  << static_cast<int>(k.vma_freshness) << ','
                  << st << ','
                  << p.sample_count << ','
                  << p.mean_cycles << ','
                  << p.stddev() << ','
                  << p.cv() << ','
                  << p.replay_count << ','
                  << p.memory_trace.size() << ','
                  << p.resample_count << ','
                  << p.drift_in_band << ','
                  << p.drift_out_band << ','
                  << p.drift_resets << ','
                  << p.drift_last_sample << ','
                  << p.quantile(0.50) << ','
                  << p.quantile(0.90) << ','
                  << p.quantile(0.99) << ','
                  << p.pcp_hit_samples << ','
                  << p.pcp_miss_samples << ','
                  << p.pcp_unknown_samples << '\n';
            }
        }
        auto& s = m_fast_fault_registry.stats();
        LineOut() << "[MimicOS] FastFault stats: profiles=" << m_fast_fault_registry.size()
                  << " trained=" << s.trained_promotions
                  << " abandoned=" << s.abandoned_promotions
                  << " detailed_trainings=" << s.detailed_trainings
                  << " fast_replays=" << s.fast_replays << std::endl;
    }
}

/* Where the kernel's report files go (<base>_scheduler_stats.csv and the
   like): the output path given on its command line, else a directory of its
   own made once per process under TMPDIR -- never a fixed name under /tmp,
   which two kernels running at once (two suites, two users) wrote over each
   other's. */
std::string MimicOS::reportBase()
{
    if (!path_to_outputFile.empty())
        return path_to_outputFile;
    static std::string made;
    if (made.empty())
    {
        const char *tmp = getenv("TMPDIR");
        std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/mimicos-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end()); buf.push_back(0);
        made = mkdtemp(buf.data()) ? std::string(buf.data()) + "/mimicos" : std::string("mimicos");
        LineOut() << "[MimicOS]: report files under " << made << "*" << std::endl;
    }
    return made;
}

void MimicOS::boot()
{
    // Open a debug log file (stdout is piped to SDE and may not be visible)
    std::ofstream boot_log(reportBase() + "_userspace_debug.log", std::ios::trunc);
    boot_log << "[MimicOS-userspace] boot() called" << std::endl;

    double target_fragmentation = reader->GetReal("pmem_alloc"    , "target_fragmentation", 1.0);
    LineOut() << "[MimicOS]: Fragmenting memory w/ target_fragmentation factor = " << target_fragmentation << std::endl;
    boot_log << "[MimicOS-userspace] target_fragmentation=" << target_fragmentation << std::endl;

    //Fragment memory to achieve target fragmentation
    // This is optional, depending on the configuration parameter 'target_fragmentation'
    // If target_fragmentation = 1.0 => no fragmentation is applied
    // If target_fragmentation < 1.0 => fragmentation is applied to achieve the target
    physical_memory_allocator->fragment_memory(target_fragmentation);


    //CRITICAL: MimicOS starts like an actual OS, we need to spawn a process
    // In our scenario, to simplify things, we spawn a single process but providing the path to an existing trace
    // Start OS daemons BEFORE the application so their threads exist
    // when SDE starts recording. The POSIX threads created here will be
    // instrumented by SDE and simulated by Sniper.
    LineOut() << "[MimicOS]: Starting OS daemons" << std::endl;
    boot_log << "[MimicOS-userspace] calling start_daemons()" << std::endl;
    boot_log.flush();
    start_daemons();
    boot_log << "[MimicOS-userspace] start_daemons() returned" << std::endl;
    boot_log.flush();

    /* ---- Stage D (Apr 17 2026): per-core app list ----
       Build m_app_paths from [apps] INI section if present, otherwise
       fall back to the single cmdline path_to_app.  This supports:
         - Single trace:  path_to_app = rnd.sift
         - Single live:   path_to_app = live:/path/to/binary
         - Multi/mixed:   [apps] section with per-core entries
       num_kernel_cores defaults to the number of apps listed. */
    int ini_num_apps = reader->GetInteger("apps", "num_apps", 0);
    if (ini_num_apps > 0) {
        for (int i = 0; i < ini_num_apps; i++) {
            char key[16]; snprintf(key, sizeof(key), "app%d", i);
            std::string app = reader->Get("apps", key, "");
            if (app.empty()) {
                std::cerr << "[MimicOS] ERROR: [apps] " << key << " missing in INI" << std::endl;
                exit(1);
            }
            m_app_paths.push_back(app);
        }
    } else {
        /* Fallback: single app from cmdline. */
        m_app_paths.push_back(path_to_app);
    }
    m_num_cores = (int)m_app_paths.size();
    m_cores.resize(m_num_cores);
    for (int i = 0; i < m_num_cores; i++) m_cores[i].core_id = i;

    /* Stage 5 (Apr 19 2026): scheduler's num_cores defaults to 1 in init()
       because [apps] is read here in boot() (after init()).  Re-configure
       if the enabled policy needs per-core runqueues for every kernel
       pthread.  Safe to call configure() here: no tasks have been created
       yet — kernel_worker is the first task creator and runs after this. */
    if (m_scheduler.is_enabled() && m_num_cores > 1) {
        SchedulerConfig scfg_patched;
        /* Read the user's INI settings again so we don't clobber policy /
           timeslice choices. */
        std::string policy_str = std::string(reader->Get("scheduler", "policy", "disabled").c_str());
        if (policy_str == "fifo")         scfg_patched.policy = SchedPolicy::FIFO_SIMPLE;
        else if (policy_str == "rr")      scfg_patched.policy = SchedPolicy::RR_SIMPLE;
        else if (policy_str == "cfs")     scfg_patched.policy = SchedPolicy::CFS_LITE;
        else if (policy_str == "vm_aware") scfg_patched.policy = SchedPolicy::VM_AWARE;
        scfg_patched.num_cores = m_num_cores;
        scfg_patched.default_timeslice_ns = reader->GetInteger("scheduler", "timeslice_ns", 4000000);
        scfg_patched.min_granularity_ns = reader->GetInteger("scheduler", "min_granularity_ns", 1000000);
        scfg_patched.yield_after_fault = reader->GetBoolean("scheduler", "yield_after_fault", true);
        scfg_patched.yield_instruction_quantum = reader->GetInteger("scheduler", "yield_instruction_quantum", 0);
        scfg_patched.prefer_numa_local = reader->GetBoolean("scheduler", "prefer_numa_local", true);
        scfg_patched.avoid_fault_heavy = reader->GetBoolean("scheduler", "avoid_fault_heavy", true);
        m_scheduler.configure(scfg_patched);
        fprintf(stderr, "[MimicOS] scheduler reconfigured for %d cores (policy=%s)\n",
                m_num_cores, policy_str.c_str());
    }

    /* Count how many live apps need the per-thread ROI barrier. */
    int num_live = 0;
    bool has_trace = false;
    for (auto& p : m_app_paths) {
        if (p.rfind("live:", 0) == 0) num_live++;
        else has_trace = true;
    }
    m_num_live_apps = num_live;  // visible to kernel_worker via member

    LineOut() << "[MimicOS]: Provisioning " << m_num_cores << " core(s), "
              << num_live << " live + " << (m_num_cores - num_live) << " trace" << std::endl;

    if (m_num_cores == 1 && !num_live) {
        /* Single trace-based app: legacy path (SimRoiStart + SimStartProcess). */
        LineOut() << "[MimicOS]: Calling start_application (trace-based)" << std::endl;
        start_application();
        LineOut() << "[MimicOS]: Calling poll_for_signal" << std::endl;
        /* Legacy path: always has an initial app to context-switch to. */
        poll_for_signal(/*core_id=*/0, /*has_initial_app=*/true);

        if (m_child_pid > 0 && m_app_vma_tree.size() > 0) {
            std::string base = reportBase();
            m_app_vma_tree.dump(base + "_vma_tree.log");
            fprintf(stderr, "[MimicOS] VMA tree dumped: %zu regions to %s_vma_tree.log\n",
                    m_app_vma_tree.size(), base.c_str());
        }
    } else {
        /* Multi-core and/or live apps: per-core dispatch.
           Trace-based apps contain their own SimRoiStart magic in the
           SIFT stream (recorded from the original workload run), which
           triggers setPerformance(true) when the Reader parses it.
           Live apps trigger it via SimThreadRoiStart (barrier).
           We do NOT call SimRoiStart from the kernel — that would put
           PIN in DETAILED mode before the live app forks, which breaks
           the fork+execve path (PIN re-instrumentation issues). */
        if (num_live > 0) {
            SimRoiExpect(1);  // first live app join enables perf model
        } else {
            /* Pure-trace multi-core: no live app will fire the barrier,
               and traces don't contain SimRoiStart magic instructions.
               Enable the perf model up front so detailed simulation
               actually runs.  kernel_worker skips SimWaitForRoi in this
               path (m_num_live_apps == 0). */
            SimRoiStart();
        }

        for (int i = 1; i < m_num_cores; i++) {
            m_cores[i].active = true;
            pthread_create(&m_cores[i].thread, nullptr,
                           MimicOS::kernel_worker_trampoline,
                           &m_cores[i]);
            LineOut() << "[MimicOS]: Spawned kernel pthread for core " << i << std::endl;
        }

        /* Core 0 runs on main thread. */
        m_cores[0].active = true;
        kernel_worker(0);

        /* Every worker leaves poll_for_signal on its shutdown event; wait
           for them before returning so the exit reports in ~MimicOS() see
           the final state and no worker is still touching it. */
        for (int i = 1; i < m_num_cores; i++)
            pthread_join(m_cores[i].thread, nullptr);
    }
}


void MimicOS::initHandlers() {

}


void MimicOS::start_daemons()
{
    // Read daemon config from INI file
    bool kcompactd_enabled = reader->GetBoolean("daemons", "kcompactd_enabled", false);

    if (kcompactd_enabled && physical_memory_allocator) {
        CompactdConfig cfg;
        cfg.enabled = true;
        cfg.scan_interval_us   = reader->GetInteger("daemons", "kcompactd_scan_interval_us", 10000);
        cfg.low_util_threshold  = reader->GetReal("daemons", "kcompactd_low_util_threshold", 0.25);
        cfg.high_util_threshold = reader->GetReal("daemons", "kcompactd_high_util_threshold", 0.75);
        cfg.max_pages_per_scan  = reader->GetInteger("daemons", "kcompactd_max_pages_per_scan", 512);

        m_kcompactd = new VirtuosoKcompactd(cfg);

        LineOut() << "[MimicOS] kcompactd created (interval=" << cfg.scan_interval_us << "us)" << std::endl;

        // Bind kcompactd to the allocator's data structures
        // The allocator must be ReserveTHP-like to expose getTwoMbMap()/getBuddyAllocator()
        String allocatorName = reader->Get("allocator", "memory_allocator", "").c_str();
        if (allocatorName == "reserve_thp") {
            // Cast to the concrete Virtuoso allocator type
            // (VirtuosoTHPAllocator is defined in mm/allocator_factory.h)
            auto* rthp = dynamic_cast<VirtuosoTHPAllocator*>(physical_memory_allocator);
            if (rthp) {
                m_kcompactd->bind(&rthp->getTwoMbMap(), rthp->getBuddyAllocator());
                m_kcompactd->start();
                LineOut() << "[MimicOS] kcompactd daemon thread started (SDE-instrumented)" << std::endl;
            } else {
                LineOut() << "[MimicOS] kcompactd: failed to bind to ReserveTHP allocator" << std::endl;
            }
        } else {
            LineOut() << "[MimicOS] kcompactd: allocator '" << allocatorName << "' does not support 2MB region tracking" << std::endl;
        }
    }

    // khugepaged
    bool khugepaged_enabled = reader->GetBoolean("daemons", "khugepaged_enabled", false);
    if (khugepaged_enabled && physical_memory_allocator) {
        KhugepagedConfig cfg;
        cfg.enabled = true;
        cfg.scan_interval_us       = reader->GetInteger("daemons", "khugepaged_scan_interval_us", 10000);
        cfg.promotion_threshold    = reader->GetReal("daemons", "khugepaged_promotion_threshold", 0.95);
        cfg.max_promotions_per_scan = reader->GetInteger("daemons", "khugepaged_max_promotions_per_scan", 16);

        m_khugepaged = new VirtuosoKhugepaged(cfg);

        String allocatorName = reader->Get("allocator", "memory_allocator", "").c_str();
        if (allocatorName == "reserve_thp") {
            auto* rthp = dynamic_cast<VirtuosoTHPAllocator*>(physical_memory_allocator);
            if (rthp) {
                m_khugepaged->bind(&rthp->getTwoMbMap());
                m_khugepaged->start();
                LineOut() << "[MimicOS] khugepaged daemon thread started" << std::endl;
            }
        }
    }

    // kswapd
    bool kswapd_enabled = reader->GetBoolean("daemons", "kswapd_enabled", false);
    if (kswapd_enabled) {
        KswapdConfig cfg;
        cfg.enabled = true;
        cfg.scan_interval_us = reader->GetInteger("daemons", "kswapd_scan_interval_us", 10000);
        cfg.pages_high       = reader->GetInteger("daemons", "kswapd_pages_high", 8192);
        cfg.pages_low        = reader->GetInteger("daemons", "kswapd_pages_low", 4096);
        cfg.pages_min        = reader->GetInteger("daemons", "kswapd_pages_min", 1024);
        cfg.batch_size       = reader->GetInteger("daemons", "kswapd_batch_size", 32);

        m_kswapd = new VirtuosoKswapd(cfg);
        m_kswapd->bind(nullptr, 0);
        m_kswapd->start();
        LineOut() << "[MimicOS] kswapd daemon thread started" << std::endl;
    }
}

void MimicOS::stop_daemons()
{
    if (m_kcompactd) {
        m_kcompactd->stop();
        LineOut() << "[MimicOS] kcompactd stopped. Scans=" << m_kcompactd->getStats().scans << std::endl;
        delete m_kcompactd;
        m_kcompactd = nullptr;
    }
    if (m_khugepaged) {
        m_khugepaged->stop();
        LineOut() << "[MimicOS] khugepaged stopped. Scans=" << m_khugepaged->getStats().scans
                  << " Promoted=" << m_khugepaged->getStats().regions_promoted << std::endl;
        delete m_khugepaged;
        m_khugepaged = nullptr;
    }
    if (m_kswapd) {
        m_kswapd->stop();
        LineOut() << "[MimicOS] kswapd stopped. Scans=" << m_kswapd->getStats().scans
                  << " Reclaimed=" << m_kswapd->getStats().pages_reclaimed << std::endl;
        delete m_kswapd;
        m_kswapd = nullptr;
    }
}

void MimicOS::start_application()
{
    const char *path = path_to_app.c_str();
    LineOut() << "[MimicOS] [start_application]: path = " << path << std::endl;

    /* Stage A (Apr 15 2026): if path begins with "live:", treat the
       remainder as a real binary and fork+execve it under PIN/SDE
       instead of attaching a pre-recorded SIFT trace.

       Phase 8 (Apr 23 2026): extended syntax with '#'-separated argv
       passthrough.  Syntax:
           live:<binary>[#arg1[#arg2[...]]]
       Examples:
           live:microbench/fault/anon_write_calib
           live:microbench/fault/anon_write_calib#-s#4096
           live:microbench/stream/stream_write#-n#1000000#-w
       The target binary sees argv[0]=binary, argv[1]=-s, argv[2]=4096
       etc.  '#' was chosen over ':' to avoid conflicting with the
       scheme prefix parser, and over ' ' because the command line
       passes the live: string as a single token through Sniper's
       argument plumbing.  Any binary that accepts `-s <bytes>` or
       similar flags can now be driven through the kernel-fault
       pipeline without code changes. */
    if (path_to_app.rfind("live:", 0) == 0) {
        std::string rest = path_to_app.substr(5);
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos < rest.size()) {
            size_t sep = rest.find('#', pos);
            if (sep == std::string::npos) {
                parts.push_back(rest.substr(pos));
                break;
            }
            parts.push_back(rest.substr(pos, sep - pos));
            pos = sep + 1;
        }
        std::string binary = parts.empty() ? rest : parts[0];
        std::vector<std::string> forwarded_args(
            parts.size() > 1 ? parts.begin() + 1 : parts.end(),
            parts.end());
        LineOut() << "[MimicOS] live: binary=" << binary
                  << " argc_passthrough=" << forwarded_args.size() << std::endl;
        for (size_t i = 0; i < forwarded_args.size(); i++) {
            LineOut() << "  argv[" << (i+1) << "] = " << forwarded_args[i] << std::endl;
        }
        SimRoiExpect(1);
        spawn_live_application(binary, forwarded_args);
        return;
    }

    //Execute an instruction which we call "magic"
    // r11 = *pointer_to_file OR opcode
    LineOut() << "[MimicOS] [start_application]: Before SimStartProcess" << std::endl;
    SimSetThreadName("mimicos_pre_SimRoiStart");
    SimRoiStart();
    SimSetThreadName("mimicos_pre_SimStartProcess");

    //Set up the magic instruction, the simulator will intercept it and start executing the SIFT-based application

    SimStartProcess((long unsigned int)(path));
    SimSetThreadName("mimicos_post_SimStartProcess");

    LineOut() << "[MimicOS] [start_application]: After SimStartProcess" << std::endl;

    /* If the scheduler is enabled, register the spawned application as a Task
       so the scheduler has a real runnable entity to track.  Per-fault
       charging happens inside poll_for_signal(). */
    if (m_scheduler.is_enabled()) {
        /* Register the task against the application's real thread id, the way
           the live-spawn path does.  create_task() leaves app_thread_id at 0,
           and the scheduler hands that straight to SimContextSwitchTo, so the
           kernel asks for a thread that was never registered: nothing is
           installed, the application never runs, and the run hangs.  That was
           hidden while id 0 happened to name a registered reader. */
        uint64_t app_id = SimGetInjectedAppId();
        uint64_t main_atid = (app_id == (uint64_t)-1) ? 0 : (app_id << 16);
        Task* app_task = m_scheduler.create_task_for_atid(path_to_app, main_atid, 0);
        if (!app_task) app_task = m_scheduler.create_task(path_to_app);
        m_scheduler.wake_up(app_task, 0);
        m_scheduler.schedule(0);
        LineOut() << "[MimicOS] Registered app trace as Task id="
                  << app_task->task_id << " atid=0x" << std::hex << main_atid
                  << std::dec << " in scheduler" << std::endl;
    }
}

/* ==========================================================================
   Stage A (Apr 15 2026): live-binary spawn.

   Model: piggyback on the existing fork/exec-follow path.  When this
   process (startup_mimicos) forks, PIN's forkAfterInChild handler opens
   new SIFT FIFOs for the child, Sniper's handleForkFunc -> createApplication
   allocates a new app_id, and newThread binds the child's m_kernel_trace
   to the shared kernel SIFT (see trace_manager.cc Stage A patch).
   We do NOT need a new magic instruction; the existing Sift::Writer::Fork
   handshake carries everything.

   The child immediately execve()s the target binary; PIN's
   followChild() returns true (see sift_recorder.cc Stage A patch) so the
   binary runs under PIN with its own SIFT stream.
   ========================================================================== */
pid_t MimicOS::spawn_live_application(const std::string& binary,
                                       const std::vector<std::string>& argv)
{
    LineOut() << "[MimicOS] [spawn_live_application]: binary=" << binary
              << " argc=" << argv.size() << std::endl;

    /* Build argv array for execve.  argv[0] conventionally = binary. */
    std::vector<const char*> raw_argv;
    raw_argv.push_back(binary.c_str());
    for (const auto& a : argv) raw_argv.push_back(a.c_str());
    raw_argv.push_back(nullptr);

    SimSetThreadName("mimicos_pre_spawn_fork");
    pid_t child = fork();
    if (child < 0) {
        std::cerr << "[MimicOS] [spawn_live_application]: fork failed: "
                  << strerror(errno) << std::endl;
        return -1;
    }
    if (child == 0) {
        /* Child: execve into the target binary.  PIN follows this exec
           (we relaxed followChild's ROI check).  Everything that was
           in startup_mimicos's image is replaced by the target. */
        SimSetThreadName("mimicos_spawn_child_pre_execve");
        extern char** environ;
        execve(binary.c_str(),
               const_cast<char* const*>(raw_argv.data()),
               environ);
        /* If execve returns, it failed. */
        std::cerr << "[MimicOS] [spawn_live_application child]: execve failed: "
                  << strerror(errno) << std::endl;
        _exit(127);
    }

    /* Parent: note the spawn, register as a scheduler Task if enabled.
       The new app_id Sniper assigned is not visible to us directly from
       userspace; we track by binary name. */
    SimSetThreadName("mimicos_post_spawn_fork");
    m_child_pid = child;
    LineOut() << "[MimicOS] [spawn_live_application]: forked child pid=" << child
              << " for binary " << binary << std::endl;

    if (m_scheduler.is_enabled()) {
        Task* t = m_scheduler.create_task(binary);
        m_scheduler.wake_up(t, 0);
        LineOut() << "[MimicOS] [spawn_live_application]: registered scheduler Task id="
                  << t->task_id << " for live binary" << std::endl;
    }

    return child;
}


/**
 * @brief Polls for signals from the SIFT-based application and handles memory allocation requests.
 * 
 * This function implements the main message handling loop for MimicOS. It performs an initial
 * context switch to halt and wait for the SIFT-based application to send memory allocation
 * requests (typically triggered by page faults). The function continuously receives messages,
 * processes memory allocation requests, and sends responses back to the application.
 * 
 * Message Protocol:
 * - Incoming: [exception_type_code, virtual_address, num_requested_frames, ...]
 * - Outgoing: [exception_type_code, vpn, physical_address, page_size, frame1, frame2, ...]
 * 
 * @details The function:
 * 1. Performs initial context switch to synchronize with SIFT application
 * 2. Enters infinite loop to handle incoming memory requests
 * 3. Decodes exception type and extracts virtual page number (VPN)
 * 4. Allocates requested number of physical memory frames (data + page table frames)
 * 5. Sends allocation results back to the requesting application
 * 6. Performs context switch to return control to the application
 * 
 * @note This function runs indefinitely and should be called from the main MimicOS thread.
 * @note Fatal error occurs if physical memory allocation fails.
 * 
 * @see SimContextSwitch(), SimReceiveMessage(), SimMimicosResult()
 * @see PhysicalMemoryAllocator::allocate(), PhysicalMemoryAllocator::handle_page_table_allocations()
 */

/* Inline timestamp helper.
 *
 * Uses rdtsc directly because under SDE/PIN, chrono::steady_clock backs onto
 * clock_gettime() which Pin's syscall emulator replaces with a stub returning
 * constant 1 — collapsing every fault phase to zero duration.  rdtsc, by
 * contrast, is intercepted by Sniper's SIFT recorder (sift/recorder/emulation.cc
 * handleRdtsc) and returns the SIMULATED cycle count, which is exactly what we
 * want for calibration.  On real hardware, rdtsc returns the host TSC which is
 * also time-proportional.  Unit: CPU cycles (not ns).
 */
static inline uint64_t mimicos_now_ns() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void MimicOS::poll_for_signal(int core_id, bool has_initial_app)
{
    // Keep the message structure to receive messages and keep it alive and as simple as possible
    Message* msg = new Message;
    msg->argv = new uint64_t[10];

    const bool instrumented = m_fault_calibration.phase_instrumentation_enabled;
    const bool replay_enabled = m_fault_replay.is_enabled();

    /* Pre-allocated frame buffer — avoids heap allocation per fault.
       MAX_ARGS=10 is the upper bound on frames per fault. */
    UInt64 frame_buf[MAX_ARGS];

    /* Linux-mimicking phase state (alloc once, reuse across faults). */
    static LinuxPhase::VmaCache         linux_vma_cache;
    static LinuxPhase::PteInstallState  linux_pte_state;
    static LinuxPhase::BookkeepingState linux_bk_state;
    static LinuxPhase::TrapEntryState   linux_trap_state;
    static LinuxPhase::TlbSyncState     linux_tlb_state;
    /* Stage 5 (Apr 19 2026) remediation (2): HW-trap + kernel-entry cost
       model.  See LinuxPhase::kernel_entry_hw_trap. */
    static LinuxPhase::KernelEntryState linux_kentry_state;
    /* Stage 5 (Apr 19 2026) remediation (2): cold-page pool for zero_fill.
       Sized to exceed the simulated LLC so each clear_page call writes
       cache-cold lines (matching the first-touch semantics of a real
       freshly-allocated anon frame from the buddy pageset).  Lazy-init
       on first use so boot isn't slowed by a big heap alloc for
       non-calibration runs. */
    static LinuxPhase::ColdPagePool     linux_zero_pool;
    /* Apr 20 2026: compensating allocator-cost model for MimicOS -O2
       builds, where the template optimiser strips Linux-mimic cost out
       of Buddy/LinuxBuddyAnonAllocator.  Called inside the PHYS_ALLOC
       phase bracket before the real locked_allocate. */
    static LinuxPhase::PcpAllocState    linux_pcp_state;
    /* Phase 9-B (May 2026): page-table-install cost model.  Fires
       inside the PHYS_ALLOC bracket when the fault's
       pgtable_install_level (Phase 9-revisit fingerprint dimension)
       is non-zero.  Two pools so PMD and PUD chains don't share
       cache lines.  Charges +3300 cyc at level 1, +6700 cyc total
       at level 2 to match real-Linux fault_class_bench deltas. */
    static LinuxPhase::PgtableInstallState linux_pgtable_state;
    /* Phase 9-E (May 2026): PCP buddy-refill cost model.  Fires AFTER
       locked_allocate inside the PHYS_ALLOC bracket when the
       allocator reports a PCP miss (last_alloc_fastpath == 0).
       Charges +7500 cyc to match the empirical eBPF measurement
       (kratos20: PCP miss p50 11 642 cyc vs PCP hit 4 092 cyc, Δ
       +185%).  Independent pool from PcpAllocState. */
    static LinuxPhase::BuddyRefillState linux_buddy_refill_state;

    /* Toggle: do the structural Linux-mimicking work or skip it (for A/B). */
    bool mimic_linux = reader->GetBoolean("fault_calibration", "mimic_linux_phases", false);

    if (mimic_linux && linux_zero_pool.pool == nullptr) {
        size_t bytes = (size_t)reader->GetInteger(
            "fault_calibration", "zero_pool_bytes", 32 * 1024 * 1024);
        linux_zero_pool.init(bytes);
    }

    SimSetThreadName("mimicos_poll_entered");

    /* Stage 2E (Apr 18 2026): the legacy initial SimContextSwitch handed
       control to the injected app reader immediately.  This is correct
       when THIS core already has a live app scheduled (has_initial_app ==
       true) — the single-threaded live-app smoke path relies on it.  For
       idle kernel pthreads we skip: they start in the event loop blocked
       on SimReceiveMessage, waiting for some other core to post a
       "new_thread" event. */
    if (has_initial_app) {
        fprintf(stderr, "[MimicOS] poll_for_signal core %d: child_pid=%d, about to SimContextSwitch\n",
                core_id, (int)m_child_pid);
        SimSetThreadName("mimicos_pre_first_SimContextSwitch");
        SimContextSwitch();
        SimSetThreadName("mimicos_post_first_SimContextSwitch");

        fprintf(stderr, "[MimicOS] poll_for_signal core %d: returned from SimContextSwitch\n", core_id);
    } else {
        fprintf(stderr, "[MimicOS] poll_for_signal core %d: idle, entering event loop without SimContextSwitch\n",
                core_id);
    }

    /* Stage C v2 (Apr 16 2026): snapshot the live app's VMA tree from
       /proc/<pid>/maps ONCE here — after the first context switch but
       before the fault loop.  This read is NOT inside any rdtsc bracket
       so it doesn't pollute per-fault latency.  Refreshed lazily on
       cache miss inside the loop (also outside the rdtsc bracket). */
    if (m_child_pid > 0) {
        fprintf(stderr, "[MimicOS] Reading /proc/%d/maps...\n", (int)m_child_pid);
        int nvma = m_app_vma_tree.refresh(m_child_pid);
        fprintf(stderr, "[MimicOS] VMA tree loaded: %d regions from /proc/%d/maps\n",
                nvma, (int)m_child_pid);
    }

    while (true) {
        SimSetThreadName("mimicos_pre_SimReceiveMessage");

        //Receive message from the SIFT-based application
        // The message contains the exception type, virtual address, and number of requested frames
        // We reuse the same message structure to keep things simple
        // msg->argv[0] = exception_type_code
        // msg->argv[1] = virtual_address
        // msg->argv[2] = num_requested_frames (data frame + page table frames)
        // msg->argv[3..] = unused

        SimReceiveMessage(&msg->argc, msg->argv);
        SimSetThreadName("mimicos_post_SimReceiveMessage");
        assert(msg->argc >= 1);
        int event_type = msg->argv[0];

        /* Stage 4 (Apr 18 2026): shutdown event — Sniper has called
           TraceManager::stop() and there's nothing left for this
           kernel pthread to do.  Return from poll_for_signal so
           kernel_worker exits and the pthread terminates. */
        if (event_type == 6) {
            fprintf(stderr, "[MimicOS] kernel %d: shutdown event received, exiting poll_for_signal\n",
                    core_id);
            return;
        }

        /* Stage 2E (Apr 18 2026): dispatch on event type.
             1 = page_fault  (existing fault-handling path below)
             3 = new_thread  (argv[1] = app_thread_id) — schedule it
             4 = thread_exit (argv[1] = app_thread_id) — remove from queue
           For v1 we handle new_thread here inline; page_fault flows through
           the existing logic.  thread_exit will come in a later step. */
        if (event_type == 3) {
            /* new_thread event: a live-app spawned a pthread that the
               Sniper-side handleNewThreadFunc scheduled onto THIS core.
               argv[1] is the encoded atid.  Stage 5: route the dispatch
               decision through Scheduler so policy (RR/CFS/VM-aware)
               actually steers who runs.  When scheduler is disabled we
               fall back to the Stage 4 per-core FIFO deque. */
            uint64_t atid = msg->argv[1];
            PerCoreState& self = m_cores[core_id];
            uint64_t next_atid = 0;
            if (m_scheduler.is_enabled()) {
                char name[64];
                snprintf(name, sizeof(name), "live_atid_%lx", (unsigned long)atid);
                Task* t = m_scheduler.create_task_for_atid(name, atid, core_id);
                if (!t) t = m_scheduler.find_task_by_atid(atid);
                if (t) m_scheduler.wake_up(t, core_id);
                Task* next = m_scheduler.schedule_and_get_current(core_id);
                if (next) next_atid = next->app_thread_id;
            } else {
                self.runqueue.push_back(atid);
                next_atid = self.runqueue.front();
            }
            fprintf(stderr, "[MimicOS] kernel %d: new_thread event, atid=0x%lx -> SimContextSwitchTo(0x%lx)\n",
                    core_id, (unsigned long)atid, (unsigned long)next_atid);
            if (next_atid) SimContextSwitchTo(next_atid);
            /* After SimContextSwitchTo, this core's TraceThread is running the
               user thread.  Loop back to SimReceiveMessage — we'll block here
               until the user thread faults or exits. */
            continue;
        }

        if (event_type == 4) {
            /* Stage 4 (Apr 18 2026): thread_exit event.  argv[1] is the
               exiting app_thread_id.  Remove it from this core's runqueue
               and, if any other user thread is queued, SimContextSwitchTo
               it.  Otherwise the kernel stays idle on this core until a
               new_thread event lands or shutdown fires.
               Stage 5: scheduler path marks Task DEAD and picks next. */
            uint64_t exited_atid = msg->argv[1];
            PerCoreState& self = m_cores[core_id];
            uint64_t next_atid = 0;
            if (m_scheduler.is_enabled()) {
                Task* t = m_scheduler.find_task_by_atid(exited_atid);
                if (t) m_scheduler.mark_dead(t);
                Task* next = m_scheduler.schedule_and_get_current(core_id);
                if (next) next_atid = next->app_thread_id;
            } else {
                for (auto it = self.runqueue.begin(); it != self.runqueue.end(); ++it) {
                    if (*it == exited_atid) { self.runqueue.erase(it); break; }
                }
                if (!self.runqueue.empty()) next_atid = self.runqueue.front();
            }
            fprintf(stderr, "[MimicOS] kernel %d: thread_exit event, atid=0x%lx -> %s\n",
                    core_id, (unsigned long)exited_atid,
                    next_atid ? "SimContextSwitchTo" : "(idle)");
            if (next_atid) SimContextSwitchTo(next_atid);
            continue;
        }

        if (event_type == 5) {
            /* Stage 3 (Apr 18 2026): quantum expired on the running thread.
               argv[1] is the preempted app_thread_id.  Stage 5: scheduler
               path does yield() (which rotates / updates vruntime depending
               on policy) then picks next; legacy path manually rotates
               the front-to-back FIFO. */
            uint64_t preempted_atid = msg->argv[1];
            PerCoreState& self = m_cores[core_id];
            uint64_t next_atid = 0;
            if (m_scheduler.is_enabled()) {
                Task* cur = m_scheduler.find_task_by_atid(preempted_atid);
                if (cur) m_scheduler.yield(cur);
                Task* next = m_scheduler.schedule_and_get_current(core_id);
                if (next) next_atid = next->app_thread_id;
            } else {
                if (!self.runqueue.empty() && self.runqueue.front() == preempted_atid) {
                    self.runqueue.pop_front();
                    self.runqueue.push_back(preempted_atid);
                } else if (!self.runqueue.empty()) {
                    auto f = self.runqueue.front();
                    self.runqueue.pop_front();
                    self.runqueue.push_back(f);
                }
                if (!self.runqueue.empty()) next_atid = self.runqueue.front();
            }
            fprintf(stderr, "[MimicOS] kernel %d: quantum_expired preempted=0x%lx -> %s(0x%lx)\n",
                    core_id, (unsigned long)preempted_atid,
                    next_atid ? "SimContextSwitchTo" : "(idle)",
                    (unsigned long)next_atid);
            if (next_atid) SimContextSwitchTo(next_atid);
            continue;
        }

        /* Page fault path (event_type == 1).  argc must be >=2 for fault
           messages carrying va + num_requested_frames. */
        assert(msg->argc >= 2);
        int exception_type_code = event_type;
        IntPtr va = msg->argv[1];
        IntPtr vpn = (va >> BASE_PAGE_SHIFT);
        int num_requested_frames = msg->argv[2];

        /* VMA lookup for the faulting address.  Cache-miss refresh
           happens HERE, before any rdtsc timing starts. */
        const ProcVma *fault_vma = nullptr;
        if (m_child_pid > 0) {
            fault_vma = m_app_vma_tree.find(va);
            if (!fault_vma) {
                /* App probably did a new mmap since our last snapshot. */
                m_app_vma_tree.refresh(m_child_pid);
                fault_vma = m_app_vma_tree.find(va);
            }
        }

        /* ---- Replay fast-path check ---- */
        if (replay_enabled) {
            FaultFingerprint fp = m_fault_replay.build_fingerprint(
                va,
                FaultMappingType::ANON,       /* default: anonymous */
                FaultAccessType::WRITE,       /* default: write */
                FaultPageSize::PAGE_4K,       /* default: 4KB target */
                0,                            /* TODO: check existing PT levels */
                0,                            /* allocator type: 0=baseline */
                0.0,                          /* TODO: query frag ratio */
                0,                            /* NUMA node */
                1,                            /* thread count */
                0                             /* contention level */
            );

            FaultReplayResult replay_result;
            if (m_fault_replay.try_replay(fp, replay_result)) {
                /* Fast path: use cached result.
                   In structured mode, we still allocate pages.
                   In analytical mode, we skip allocation entirely. */

                UInt64 pa_replay;
                UInt64 ps_replay;

                /* Both modes still call allocate() to keep state consistent.
                   Structured mode materialises all state; analytical charges cost only.
                   The real speedup comes from skipping detailed control-flow exploration
                   and from cache lookup being cheaper than the full instrumented path. */
                auto [pa_rp, ps_rp] = locked_allocate((1 << 12), va, 0);
                if (pa_rp == static_cast<UInt64>(-1)) {
                    std::cerr << "[FATAL] [MimicOS] Replay path: allocation failed" << std::endl;
                    exit(1);
                }
                pa_replay = pa_rp;
                ps_replay = ps_rp;

                /* Build response from replay using stack buffer */
                int rp_count = 0;
                frame_buf[rp_count++] = pa_replay;
                for (int i = 0; i < num_requested_frames - 1; i++) {
                    frame_buf[rp_count++] =
                        locked_handle_pt_alloc(1 << 12);
                }

                msg->argc = 4 + num_requested_frames;
                msg->argv[0] = exception_type_code;
                msg->argv[1] = vpn;
                msg->argv[2] = pa_replay;
                msg->argv[3] = ps_replay;
                for (int i = 0; i < rp_count; i++) {
                    msg->argv[4 + i] = frame_buf[i];
                }

                /* Record replayed fault in metrics too — we charge the cached
                   cost as the total.  This keeps total fault_count accurate
                   across detailed + replay paths. */
                if (instrumented) {
                    FaultPhaseTimings t;
                    t.begin_fault(0, va);
                    t.end_fault(replay_result.charged_cost_ns);
                    t.fault_class.mapping = FaultMappingType::ANON;
                    t.fault_class.access  = FaultAccessType::WRITE;
                    t.fault_class.page_size = (ps_replay == 21)
                        ? FaultPageSize::PAGE_2M : FaultPageSize::PAGE_4K;
                    t.fault_class.pt_alloc_needed = (num_requested_frames > 1);
                    m_fault_metrics.record(t);
                }

                SimSetThreadName("mimicos_replay_pre_SimMimicosResult");
                SimMimicosResult(msg->argc, msg->argv);
                SimSetThreadName("mimicos_replay_post_SimMimicosResult");

                /* Scheduler: charge replayed fault cost to the currently-
                   running app Task (the one that just faulted).  Multi-app:
                   this gives correct per-Task attribution. */
                if (m_scheduler.is_enabled()) {
                    Task* t = m_scheduler.current_task(core_id);   /* this core, under its lock */
                    if (!t) t = m_scheduler.get_task(0);  /* single-task fallback */
                    if (t) m_scheduler.on_page_fault(t, replay_result.charged_cost_ns);
                }

                SimContextSwitch();
                SimSetThreadName("mimicos_replay_post_loop_SimContextSwitch");

                /* Periodic dump on the replay path too — driven by replay-cache
                   total_lookups so we get persistent output even when most
                   faults skip the detailed path. */
                {
                    uint64_t lookups = m_fault_replay.cache_stats().total_lookups();
                    if (lookups > 0 && (lookups % 50) == 0) {
                        std::string base = reportBase();
                        m_fault_replay.dump_stats(base + "_replay_stats.csv");
                        if (instrumented && m_fault_metrics.fault_count() > 0) {
                            m_fault_metrics.dump_csv(base + "_fault_phases.csv");
                            m_fault_metrics.dump_histograms(base + "_fault_histograms.csv");
                        }
                        if (m_scheduler.is_enabled()) {
                            m_scheduler.dump_stats(base + "_scheduler_stats.csv");
                        }
                    }
                }
                continue;  /* skip the detailed path below */
            }
            /* Fall through to detailed path */
        }

        /* ---- Phase instrumentation: begin fault ---- */
        FaultPhaseTimings timings;
        if (instrumented) {
            timings.begin_fault(mimicos_now_ns(), va);
        }

#if DEBUG_MimicOS >= DEBUG_BASIC
        LineOut() << "[MimicOS] Received response from context switch ..." << std::endl;
        LineOut() << "[MimicOS] Message is exception_type = " << exception_type_code <<
                     " -- vpn = " << vpn <<
                     " -- num_requested_frames = " << num_requested_frames << std::endl;
#endif

        #ifdef PROTOCOL
            assert(argc >= 1);
            std::string message_type;
            if (protocol_codes_decode.find(argv[0]) == protocol_codes_decode.end()) {
                LineOut() << "[MimicOS]: Unknown protocol code: " << argv[0] << std::endl;
                continue; // Skip unknown messages
            }
            else {
                message_type = protocol_codes_decode[argv[0]];
                LineOut() << "[MimicOS]: protocol name / message type : " << message_type << std::endl;
            }
        #endif

        /* ---- Fast-fault trace capture: arm the thread-local target iff
               this fingerprint is about to see its first TRAINING sample.
               The fingerprint here uses Phase-2 pcp_hit_or_miss=unknown;
               once a pcp-telemetry hook lands, this pre-phase fingerprint
               will split into the hit / miss classes.  The registry still
               records the per-sample statistics at end-of-fault via the
               same fingerprint below. ---- */
        FastFaultProfile* capture_prof = nullptr;
        FastFaultProfile* replay_prof = nullptr;
        bool fast_replay_mode = false;
        bool drift_resample = false;  /* Phase 7: detailed run triggered by resample_every */
        /* Phase 9: snapshot the predictor value at the TOP of the
           fault so pre- and post-fault fingerprint builds use the
           same pcp class, even though the allocator will update
           m_last_pcp_class mid-fault.  Without this snapshot, pre
           uses last-fault's class and post uses this-fault's class,
           and the Welford sample lands in a DIFFERENT profile from
           the one we fast-replayed from. */
        uint8_t this_fault_pcp_class = m_last_pcp_class;
        /* Phase 9 revisit: snapshot the pgtable install level at the TOP
           of the fault (read-only — we don't insert into the tracker
           until the fault retires).  Same snapshot pattern as
           this_fault_pcp_class so pre/post fingerprints stay aligned. */
        uint8_t this_fault_pgt_level =
            m_fast_fault_registry.policy().hierarchical_split_pgtable_enabled
                ? compute_pgtable_install_level(static_cast<uint64_t>(va))
                : 0;
        /* Phase 9-C: snapshot vma_freshness from the per-process
           last-fault-VPN tracker.  Mutually exclusive with
           pgtable_install_level > 0 — when the dispatcher detects an
           install event, that wins and we force vma_freshness=aged
           to keep the cells from double-classifying.  Default 1 (aged)
           when the knob is off so the fingerprint is stable. */
        uint8_t this_fault_vma_freshness = 1;
        if (m_fast_fault_registry.policy().hierarchical_split_vma_freshness_enabled) {
            if (this_fault_pgt_level > 0) {
                this_fault_vma_freshness = 1;  // install events override
            } else {
                this_fault_vma_freshness =
                    compute_vma_freshness(static_cast<uint64_t>(vpn));
            }
        }
        if (m_fast_fault_registry.is_enabled()) {
            FaultFingerprint fp_pre = m_fault_replay.build_fingerprint(
                va, FaultMappingType::ANON, FaultAccessType::WRITE,
                FaultPageSize::PAGE_4K, 0, 0, 0.0, 0, 1, 0);
            /* Phase 9: inject predicted pcp class so lookups partition
               into per-class child profiles.  Default FaultFingerprint
               has pcp_hit_or_miss = 2 (unknown); we overwrite here iff
               splitting is enabled.  Once a single fault has run, the
               predictor (this_fault_pcp_class) flips from 2 to 0/1 and
               subsequent faults route to the hit or miss child. */
            if (m_fast_fault_registry.policy().hierarchical_split_enabled) {
                fp_pre.pcp_hit_or_miss = this_fault_pcp_class;
            }
            if (m_fast_fault_registry.policy().hierarchical_split_pgtable_enabled) {
                fp_pre.pgtable_install_level = this_fault_pgt_level;
            }
            if (m_fast_fault_registry.policy().hierarchical_split_vma_freshness_enabled) {
                fp_pre.vma_freshness = this_fault_vma_freshness;
            }
            FastFaultProfile& prof = m_fast_fault_registry.get_or_insert(fp_pre);
            switch (prof.state) {
                case FastFaultState::UNKNOWN:
                case FastFaultState::TRAINING:
                    if (prof.memory_trace.empty()) {
                        capture_prof = &prof;
                        LinuxPhase::g_capture_target = capture_prof;
                    }
                    break;
                case FastFaultState::TRAINED: {
                    /* Phase 7 drift-detection: periodically re-sample in
                       detailed mode.  Cadence is the configured
                       resample_every (0 = disabled).  We run detailed
                       WITHOUT arming capture — the trace is frozen post-
                       promotion and doesn't need refreshing on every
                       drift check; only a cost sample is needed. */
                    const auto& pol = m_fast_fault_registry.policy();
                    if (pol.resample_every > 0 &&
                        prof.replay_count > 0 &&
                        (prof.replay_count % pol.resample_every) == 0) {
                        drift_resample = true;
                        replay_prof = &prof;  /* held for post-fault drift check */
                        /* fall through with fast_replay_mode = false */
                    } else {
                        replay_prof = &prof;
                        fast_replay_mode = true;
                    }
                    break;
                }
                case FastFaultState::ABANDONED:
                    break;
            }
        }

        /* ---- Phase: TRAP ENTRY (Linux do_user_addr_fault prologue) ---- */
        if (instrumented)
            timings.begin_phase(FaultPhase::FAULT_TRAP_ENTRY, mimicos_now_ns());
        if (mimic_linux && !fast_replay_mode) {
            uint64_t fake_err = (uint64_t)exception_type_code | 0x6;  /* USER|WRITE */
            /* HW vector + kernel-entry cost (cold IDT-shadow line read +
               per-CPU perf_sw_event atomic RMW on a cache-cold slot).
               This is the chunk that fault_bench's rdtscp bracket charges
               but our phase CSV previously missed entirely.  ~150-300 cyc
               on the warm path, depending on cache state.
               Phase 9-D: skip on fresh-VMA faults — same warmth-from-mmap
               argument as pcp_alloc_cost: per-CPU perf_sw_event slots
               and IDT-shadow lines are still hot from the just-completed
               syscall, so the extra dep-chain cost doesn't apply. */
            bool skip_kentry_for_fresh =
                m_fast_fault_registry.policy().hierarchical_split_vma_freshness_enabled
                && this_fault_vma_freshness == 0;
            /* Phase 9-D: when fresh, skip BOTH kernel_entry_hw_trap and
               trap_entry — same warmth-from-mmap argument as
               pcp_alloc_cost: per-CPU perf_sw_event slots and IDT-shadow
               lines are still hot from the just-completed syscall.
               Lands fresh-fault ratio at ~0.53x (vs Linux target 0.59x),
               within the ±10 % calibration target.  Skipping only one
               of the two pair leaves a cold trap-entry chain that costs
               MORE in isolation than the warm pair — so both-or-neither
               is the right granularity. */
            if (!skip_kentry_for_fresh) {
                LinuxPhase::kernel_entry_hw_trap(linux_kentry_state, fake_err);
                LinuxPhase::trap_entry(linux_trap_state, fake_err);
            }
        }
        if (instrumented)
            timings.end_phase(FaultPhase::FAULT_TRAP_ENTRY, mimicos_now_ns());

        /* ---- Phase: VMA LOOKUP ---- */
        if (instrumented)
            timings.begin_phase(FaultPhase::FAULT_VMA_LOOKUP, mimicos_now_ns());

        UInt64 bytes = (1 << 12);

        /* The allocation is made as this worker's core (poll_for_signal's
           core_id): the Linux-style allocator keeps one page set per CPU and
           pops from it without a lock, on the assumption that only that CPU's
           worker touches it.  A local `core_id = 0` here used to send every
           worker to CPU 0's page set, and two workers handling faults at the
           same time raced on its free list: the same frame handed out twice
           (the MMU's sanity check then aborts on a PA mapped by two VAs) or
           the list corrupted. */

        /* Linux-mimicking VMA lookup: walk the small sorted VMA cache.
           Returns true if VPN was already seen (warm), false otherwise.
           Either way we pay the search cost — that's the simulated
           per-phase work we want. */
        if (mimic_linux && !fast_replay_mode) {
            volatile bool hit = linux_vma_cache.lookup((uint64_t)vpn);
            if (!hit) linux_vma_cache.note((uint64_t)vpn);
            (void)hit;
        }

        if (instrumented)
            timings.end_phase(FaultPhase::FAULT_VMA_LOOKUP, mimicos_now_ns());

        /* ---- Phase: PHYS ALLOC (data frame) ---- */
        if (instrumented)
            timings.begin_phase(FaultPhase::FAULT_PHYS_ALLOC, mimicos_now_ns());

        int frame_count = 0;

        if (mimic_linux && !fast_replay_mode) {
            /* Compensate for GCC template-instantiation optimisation that
               strips the allocator's calibrated cost under -O2.  See
               LinuxPhase::pcp_alloc_cost in linux_phase_work.h.
               Phase 9-C: skip when the fault is classified as fresh
               (vma_freshness=0) — empirically the immediately-prior
               mmap leaves the per-CPU pageset metadata hot, so the
               full pcp-walk cost doesn't apply.  Skipping this helper
               removes ~1185 cyc, approximating the −1670 cyc empirical
               Δ for fresh_vma faults. */
            bool skip_pcp_for_fresh =
                m_fast_fault_registry.policy().hierarchical_split_vma_freshness_enabled
                && this_fault_vma_freshness == 0;
            if (!skip_pcp_for_fresh) {
                LinuxPhase::pcp_alloc_cost(linux_pcp_state, va);
            }
            /* Phase 9-B: charge the page-table install Δ for faults
               on PMD/PUD boundaries.  No-op when level == 0 (steady).
               Gated on the Phase-9-revisit knob so disabling the
               splitter also disables the cost model — keeps pre-9B
               behaviour reproducible. */
            if (m_fast_fault_registry.policy().hierarchical_split_pgtable_enabled
                && this_fault_pgt_level > 0) {
                LinuxPhase::install_pgtable_cost(linux_pgtable_state,
                                                 static_cast<uint64_t>(va),
                                                 this_fault_pgt_level);
            }
        }
        auto [pa, page_size] = locked_allocate(bytes, va, core_id);
        if (pa == static_cast<UInt64>(-1)) {
            std::cerr << "[FATAL] [MimicOS] No more memory available to sustain this memory allocation" << std::endl;
            std::cerr << "[FATAL] [MimicOS] Exiting..." << std::endl;
            exit(1);
        }

        /* Phase 9: update the per-fault pcp-class predictor right
           after the allocator reports its fast-path result.  Done on
           every fault (TRAINING, TRAINED fast-replay, drift
           resample) so the predictor never staleness-locks.  Cheap
           (one byte copy); cost-free when hierarchical_split_enabled
           is false because nothing reads it. */
        if (physical_memory_allocator) {
            m_last_pcp_class = physical_memory_allocator->lastAllocFastpath(core_id);

            /* Report the path as well as recording it.  The registry's CSV
               carries pcp_class, but it is written from a shutdown path that
               does not run here -- poll_for_signal never returns, because no
               shutdown event reaches this kernel -- so without this line the
               per-CPU pool is unobservable from outside and selecting the
               allocator can only be shown to construct one. */
            {
                const char *path_name = "unknown";
                switch (m_last_pcp_class)
                {
                    case 0: path_name = "slow"; break;   /* pool refilled from buddy */
                    case 1: path_name = "fast"; break;   /* served from the per-CPU pool */
                    default: break;                      /* allocator does not say */
                }
                /* One line per fault from any worker; without the lock two
                   workers' lines interleave character by character. */
                static std::mutex log_lock;
                std::lock_guard<std::mutex> ll(log_lock);
                LineOut() << "[MimicOS] alloc_path=" << path_name
                          << " core=" << core_id << std::endl;
            }
            /* Phase 9-E: charge the buddy-refill Δ when the allocator
               reports a PCP miss.  Fires inside the PHYS_ALLOC bracket
               so the cycles land in the right phase histogram.  Gated
               on `mimic_linux && !fast_replay_mode` (same gate as
               pcp_alloc_cost), AND on the pcp split knob so disabling
               the splitter keeps pre-9E behaviour intact. */
            if (mimic_linux && !fast_replay_mode
                && m_fast_fault_registry.policy().hierarchical_split_enabled
                && m_last_pcp_class == 0) {
                LinuxPhase::buddy_refill_cost(linux_buddy_refill_state,
                                              static_cast<uint64_t>(va));
            }
        }

        /* Phase 9 revisit: now that the fault has allocated and we've
           captured the level snapshot, mark this VA's PMD/PUD as
           installed so subsequent faults in the same 2 MiB / 1 GiB
           chunk see level 0.  Idempotent insert.  Cost-free when
           hierarchical_split_pgtable_enabled is false because the
           sets are read-only at the entry compute and we only insert
           here. */
        if (m_fast_fault_registry.policy().hierarchical_split_pgtable_enabled) {
            mark_pgtable_installed(static_cast<uint64_t>(va));
        }
        /* Phase 9-C: commit the last-fault VPN so the next fault's
           freshness check has a current reference.  Cost-free when
           the knob is off because compute_vma_freshness isn't called. */
        if (m_fast_fault_registry.policy().hierarchical_split_vma_freshness_enabled) {
            mark_fault_vpn(static_cast<uint64_t>(vpn));
        }

        frame_buf[frame_count++] = pa;

        if (instrumented)
            timings.end_phase(FaultPhase::FAULT_PHYS_ALLOC, mimicos_now_ns());

        /* ---- Phase: PT ALLOC (page table frames) ---- */
        if (instrumented)
            timings.begin_phase(FaultPhase::FAULT_PT_ALLOC, mimicos_now_ns());

        for (int i = 0; i < num_requested_frames - 1; i++) {
            auto frame = locked_handle_pt_alloc(bytes);
#if DEBUG_MimicOS >= DEBUG_BASIC
            LineOut() << "[MimicOS] Allocated page table frame " << i << ": " << frame << std::endl;
#endif
            frame_buf[frame_count++] = frame;
        }

        if (instrumented)
            timings.end_phase(FaultPhase::FAULT_PT_ALLOC, mimicos_now_ns());

        /* Classify the fault — done outside any phase bracket so it doesn't
           inflate any phase's measured cost. */
        if (instrumented) {
            timings.fault_class.mapping = FaultMappingType::ANON;
            timings.fault_class.access  = FaultAccessType::WRITE;
            timings.fault_class.page_size = (page_size == 21)
                ? FaultPageSize::PAGE_2M : FaultPageSize::PAGE_4K;
            timings.fault_class.pt_alloc_needed = (num_requested_frames > 1);
            timings.fault_class.core_id = core_id;
        }

#if DEBUG_MimicOS >= DEBUG_BASIC
            LineOut() << "[MimicOS] Physical memory allocation succeeded: " << std::endl;
            LineOut() << "[MimicOS] Number of frames requested = " << num_requested_frames << std::endl;
            LineOut() << "[MimicOS] Frames allocated: ";
            for (int i = 0; i < frame_count; i++) {
                if (i == 0) {
                    LineOut() << "Data frame: ";
                } else {
                    LineOut() << "Page table frame " << (i - 1) << ": ";
                }
                LineOut() << frame_buf[i] << std::endl;
            }
#endif

        /* ---- Phase: ZERO FILL (Linux clear_page) ---- */
        if (instrumented)
            timings.begin_phase(FaultPhase::FAULT_ZERO_FILL, mimicos_now_ns());

        if (mimic_linux && !fast_replay_mode) {
            /* Mimic Linux's clear_page on a freshly-allocated (cache-cold)
               anon frame.  Previously we re-used one 4 KB scratch buffer
               and the writes stayed L1-resident ⇒ deterministic ~320 cyc,
               which understates real clear_page by ~2×.  Now we rotate
               through a pool sized above the simulated LLC so each call
               hits cold cache lines; cost tracks the cache hierarchy
               level the line last lived in (typically LLC or DRAM). */
            uint8_t* cold_frame = linux_zero_pool.next_page();
            if (cold_frame) {
                LinuxPhase::clear_page(cold_frame);
            }
        }

        if (instrumented)
            timings.end_phase(FaultPhase::FAULT_ZERO_FILL, mimicos_now_ns());

        /* ---- Phase: PTE INSTALL (build response + Linux-style work) ---- */
        if (instrumented)
            timings.begin_phase(FaultPhase::FAULT_PTE_INSTALL, mimicos_now_ns());

        msg->argc = 4 + num_requested_frames;
        msg->argv[0] = exception_type_code;
        msg->argv[1] = vpn;
        msg->argv[2] = pa;
        msg->argv[3] = page_size;
        for (int i = 0; i < frame_count; i++) {
            msg->argv[4 + i] = frame_buf[i];
        }

        if (mimic_linux && !fast_replay_mode) {
            /* Mimic Linux's set_pte_at + folio_add_new_anon_rmap +
               folio_add_lru_vma: atomic store + atomic counter inc on
               separate cache lines + a list-head write. */
            LinuxPhase::install_pte(linux_pte_state, (uint64_t)pa, (uint64_t)vpn);
        }

        if (instrumented)
            timings.end_phase(FaultPhase::FAULT_PTE_INSTALL, mimicos_now_ns());

        /* ---- Phase: TLB SYNC (Linux update_mmu_cache_range) ---- */
        if (instrumented)
            timings.begin_phase(FaultPhase::FAULT_TLB_SYNC, mimicos_now_ns());
        if (mimic_linux && !fast_replay_mode) {
            LinuxPhase::tlb_sync(linux_tlb_state);
        }
        if (instrumented)
            timings.end_phase(FaultPhase::FAULT_TLB_SYNC, mimicos_now_ns());

        /* ---- Phase: BOOKKEEPING (Linux-style atomic accounting) ---- */
        if (instrumented)
            timings.begin_phase(FaultPhase::FAULT_BOOKKEEPING, mimicos_now_ns());

        if (mimic_linux && !fast_replay_mode) {
            /* Mimic Linux's mm_account_fault + perf_sw_event + per-task
               counter updates: a few atomic adds on a single warm cache line. */
            LinuxPhase::account_fault(linux_bk_state);

            /* Phase 7 drift injection.  When configured, pad every
               detailed fault past `inject_drift_after_faults` with
               an extra cold-chain-read batch.  Fires on TRAINING
               samples (once the trigger is crossed) AND on drift
               resamples (because those also run detailed).  Never
               fires on fast-replay (which bypasses this whole
               branch via !fast_replay_mode), so the replay body
               remains cheap and deterministic. */
            if (m_fault_calibration.inject_drift_extra_iters > 0) {
                uint64_t fault_idx = m_fault_metrics.fault_count();
                if (fault_idx >= m_fault_calibration.inject_drift_after_faults) {
                    volatile uint64_t sink = LinuxPhase::inject_drift_cycles(
                        static_cast<int>(m_fault_calibration.inject_drift_extra_iters));
                    (void)sink;
                }
            }
        }

        if (instrumented)
            timings.end_phase(FaultPhase::FAULT_BOOKKEEPING, mimicos_now_ns());

        /* ---- Disarm trace capture.  After this point the profile's
               memory_trace is frozen — subsequent TRAINING samples only
               feed the Welford statistics. ---- */
        if (capture_prof) {
            LinuxPhase::g_capture_target = nullptr;
        }

        /* ---- End fault & record metrics ---- */
        if (instrumented) {
            timings.end_fault(mimicos_now_ns());
            m_fault_metrics.record(timings);
        }

        /* ---- Record detailed result into replay cache + fast-fault registry ---- */
        if (instrumented) {
            FaultFingerprint fp = m_fault_replay.build_fingerprint(
                va, FaultMappingType::ANON, FaultAccessType::WRITE,
                (page_size == 21) ? FaultPageSize::PAGE_2M : FaultPageSize::PAGE_4K,
                0, 0, 0.0, 0, 1, 0);
            /* Phase 9 (revised 9-E): record the sample into the
               profile keyed by the *actual* pcp class observed by the
               allocator on this fault, not the predictor.  This breaks
               pre/post fingerprint alignment when the predictor
               mispredicts (which is most of the time for misses,
               since they cluster — post-miss is usually hit).  The
               sample lands in the correct truth-keyed profile, so
               the hit and miss profile means cleanly reflect the
               buddy_refill_cost helper's per-fault Δ.  Trade-off:
               fast-replay is still gated by the predictor (pre-fault
               profile lookup), so a TRAINED hit profile may
               fast-replay a fault that turns out to be a miss; the
               cost-anchor analytical replay charges the hit-mean for
               that fault.  Acceptable for paper-grade per-class
               accuracy, since miss rate is ~1.6% on real Linux. */
            if (m_fast_fault_registry.policy().hierarchical_split_enabled) {
                fp.pcp_hit_or_miss = m_last_pcp_class;
            }
            /* Phase 9 revisit: same pre/post snapshot trick for pgtable
               install level.  this_fault_pgt_level was captured before
               we inserted into the tracker; we COMMIT the install
               record below so the NEXT fault in this chunk sees
               level 0.  Same pattern as the pcp predictor. */
            if (m_fast_fault_registry.policy().hierarchical_split_pgtable_enabled) {
                fp.pgtable_install_level = this_fault_pgt_level;
            }
            if (m_fast_fault_registry.policy().hierarchical_split_vma_freshness_enabled) {
                fp.vma_freshness = this_fault_vma_freshness;
            }

            if (replay_enabled) {
                m_fault_replay.record_detailed(fp, timings, pa, page_size,
                                               num_requested_frames - 1);
            }

            /* Phase 2/3 train-then-replay: update per-fingerprint profile.
               In TRAINING we ran detailed and recorded a sample; in
               fast-replay we skipped the detailed path — timings.total
               reflects only the bookkeeping phases, not real work, so
               don't pollute the Welford stats with it. */
            if (m_fast_fault_registry.is_enabled()) {
                FastFaultProfile& prof = m_fast_fault_registry.get_or_insert(fp);
                auto& s = m_fast_fault_registry.stats();

                if (fast_replay_mode) {
                    prof.replay_count++;
                    s.fast_replays++;
                } else if (drift_resample && prof.state == FastFaultState::TRAINED) {
                    /* Phase 7: detailed re-sample of a TRAINED profile for
                       drift detection.  DO NOT feed the sample back into
                       Welford — those stats are frozen post-promotion and
                       represent the training-time distribution.  Just run
                       the band-check. */
                    prof.replay_count++;          /* still counts as a "fault seen" */
                    s.fast_replays++;             /* accounting: this fault used the fast-path slot */
                    uint64_t sample = timings.total_duration();
                    auto action = m_fast_fault_registry.check_drift(prof, sample);
                    if (action == FastFaultRegistry::DriftAction::DEMOTE) {
                        /* Stop the barrier-advance in SIM_CMD_MIMICOS_RESULT
                           from charging the now-stale mean_cycles.  When
                           the profile re-trains and promotes again we'll
                           restore the charge with the new mean. */
                        SimSetFastFaultCharge(0);
                        LineOut() << "[MimicOS] FastFault DRIFT DETECTED fp_id="
                                  << prof.fp_id << " after " << prof.resample_count
                                  << " resamples (last_sample=" << prof.drift_last_sample
                                  << " threshold=" << m_fast_fault_registry.policy().drift_consecutive_out
                                  << " consecutive out) → demoted to TRAINING"
                                  << " (reset #" << prof.drift_resets << ")" << std::endl;
                    } else if (action == FastFaultRegistry::DriftAction::OUT_BAND) {
                        LineOut() << "[MimicOS] FastFault resample fp_id="
                                  << prof.fp_id << " OUT_OF_BAND sample="
                                  << prof.drift_last_sample
                                  << " mean=" << prof.mean_cycles
                                  << " sigma=" << prof.stddev()
                                  << " streak=" << prof.drift_out_streak << std::endl;
                    }
                    /* IN_BAND: silent (logged via CSV counters). */
                } else {
                    if (prof.state == FastFaultState::UNKNOWN) {
                        prof.state = FastFaultState::TRAINING;
                    }
                    FastFaultState prev = prof.state;
                    prof.record_sample(timings.total_duration());

                    /* Item C: record the allocator's reported fast-path
                       class (pcp hit / miss / unknown) on this TRAINING
                       sample.  Lets the dump CSVs show per-profile
                       bimodality evidence.  Does not yet feed into
                       fingerprint splitting — that's Phase 9 work. */
                    if (physical_memory_allocator) {
                        prof.record_pcp_class(physical_memory_allocator->lastAllocFastpath(core_id));
                    }

                    m_fast_fault_registry.maybe_promote(prof);

                    if (prof.state == FastFaultState::TRAINING) {
                        s.detailed_trainings++;
                    }
                    if (prev == FastFaultState::TRAINING &&
                        prof.state == FastFaultState::TRAINED) {
                        s.trained_promotions++;
                        /* Technique B: freeze vector capacity so the
                           data() pointer is stable for the Sniper-side
                           dictionary cache.  shrink_to_fit reallocs
                           once; all subsequent replays see the same
                           trace_ptr, making the (ptr, len) key a
                           reliable dedup handle. */
                        prof.captured_accesses.shrink_to_fit();
                        LineOut() << "[MimicOS] FastFault profile fp_id="
                                  << prof.fp_id
                                  << (prof.drift_resets > 0 ? " RE-TRAINED" : " TRAINED")
                                  << " after "
                                  << prof.sample_count << " samples (mean="
                                  << prof.mean_cycles << " cv=" << prof.cv()
                                  << " trace_len=" << prof.captured_accesses.size()
                                  << " trace_ptr=" << (void*)prof.captured_accesses.data()
                                  << " total_fault_idx=" << m_fault_metrics.fault_count()
                                  << (prof.drift_resets > 0 ? std::string(" drift_resets=") + std::to_string(prof.drift_resets) : std::string())
                                  << ")" << std::endl;
                        /* Tell the MMU to charge this profile's mean
                           latency on future faults so the app's clock
                           advances without requiring the kernel pthread
                           to run detailed LinuxPhase.  Convert ns→cyc
                           via the core frequency (2.9 GHz on kratos20,
                           so 1 ns ≈ 2.9 cyc).  We store ns in
                           mean_cycles, per timings.total_duration()
                           returning mimicos_now_ns. */
                        uint64_t cyc = static_cast<uint64_t>(prof.mean_cycles);
                        SimSetFastFaultCharge(cyc);
                    }
                    if (prev == FastFaultState::TRAINING &&
                        prof.state == FastFaultState::ABANDONED) {
                        s.abandoned_promotions++;
                        LineOut() << "[MimicOS] FastFault profile fp_id="
                                  << prof.fp_id << " ABANDONED after "
                                  << prof.sample_count << " samples (cv="
                                  << prof.cv() << ")" << std::endl;
                    }
                }
            }
        }

        /* ---- Phase 5: ship the whole trace in ONE SimFastFault magic
               instead of executing per-access real instructions.  The
               Sniper-side handler reads the CapturedAccess blob from our
               memory and issues core->accessMemory(MEM_MODELED_TIME)
               per entry — bypasses the MimicOS pthread's ROB and SIFT
               stream for the replay body entirely.  For zero_sweep
               (page init) the handler either issues 64 real line-writes
               (static_zero_cycles=0) or one line-write + a fixed shmem
               latency charge (static_zero_cycles>0), avoiding the
               SIMD-memset expansion that dominated prior-replay wall
               time.
               Payload layout (see magic_server.cc SIM_CMD_FAST_FAULT).
               volatile + asm("" ::: "memory") are required — at -O2 gcc
               would otherwise elide the payload initialisation because
               the inline asm has no "memory" clobber. */
        /* ---- Phase 5/6 win-win replay body ----
           Clock advance happens in the SimMimicosResult magic handler
           via BarrierSyncServer::advanceGlobalTime (see magic_server.cc).
           That advance charges the profile's full mean_cycles on every
           fault regardless of how much cache work we do here, so the
           SimFastFault body below is purely for cache-state fidelity.
           Phase 6 exposes three modes via the fast_fault INI section:
             - FULL       : replay every captured access (default)
             - PREFIX_K   : replay only the first K accesses (touch-once)
             - ANALYTICAL : skip the magic entirely (zero cache traffic)
           The dict_cache knob lets the Sniper handler cache the decoded
           CapturedAccess blob keyed by trace_ptr (avoids per-replay
           core->accessMemory READs of N×16 B). */
        if (fast_replay_mode && replay_prof) {
            const auto& pol = m_fast_fault_registry.policy();
            uint64_t trace_sz = replay_prof->captured_accesses.size();
            if (pol.replay_mode == FastFaultReplayMode::PREFIX_K) {
                uint64_t k = pol.replay_prefix_k;
                if (k < trace_sz) trace_sz = k;
            } else if (pol.replay_mode == FastFaultReplayMode::ANALYTICAL) {
                trace_sz = 0;  /* signal to caller below: skip magic */
            }
            if (trace_sz > 0) {
                static thread_local uint64_t s_replay_cursor = 0;
                volatile uint64_t payload[8];
                payload[0] = 27;
                payload[1] = replay_prof->fp_id;
                payload[2] = (uint64_t)pa;
                payload[3] = 0;  /* static_zero_cycles=0 → expand 64 line writes */
                payload[4] = trace_sz;
                payload[5] = reinterpret_cast<uint64_t>(
                                  replay_prof->captured_accesses.data());
                payload[6] = s_replay_cursor++;
                /* argv[7] flag word: bit 0 = dict_cache enable.  Optional
                   tail argument — handler falls back to defaults when
                   argc < 8, so old bracket binaries remain compatible. */
                payload[7] = pol.dict_cache ? 1ULL : 0ULL;
                asm volatile("" ::: "memory");
                SimFastFault(8, (uint64_t*)payload);
            }
            /* ANALYTICAL mode: nothing shipped; barrier.advanceGlobalTime
               in SimMimicosResult charges the full mean_cycles. */
        }

        //Send the response back to the SIFT-based application using a magic instruction
        SimSetThreadName("mimicos_pre_SimMimicosResult");
        SimMimicosResult(msg->argc, msg->argv);
        SimSetThreadName("mimicos_post_SimMimicosResult");

        /* Scheduler integration: charge the fault cost to the currently-
           running app Task (the one that just faulted).  Multi-app: this
           gives correct per-Task fault attribution. */
        if (m_scheduler.is_enabled()) {
            Task* t = m_scheduler.current_task(core_id);   /* this core, under its lock */
            if (!t) t = m_scheduler.get_task(0);  /* single-task fallback */
            if (t) {
                uint64_t cost = instrumented ? timings.total_duration() : 0;
                m_scheduler.on_page_fault(t, cost);
            }
        }

        //Perform context switch to return control to the SIFT-based application
        SimContextSwitch();
        SimSetThreadName("mimicos_post_loop_SimContextSwitch");

        /* Periodic metrics dump.  The poll loop is infinite and Sniper's
           stop-by-icount kills the process without calling the
           destructor, so we need to dump occasionally to always have
           recent data on disk.
           Stage 5 (Apr 19 2026): lowered cadence from every 10 faults
           to every 256 faults.  At every 10 the ofstream-formatted
           write of the 217-line fault_histograms.csv + 64-line
           fault_phases.csv was generating ~250k simulated instructions
           per dump; dividing by 10 put ~25k simulated instrs/fault on
           the hot path, which amounts to ~40% of the userspace-MimicOS
           per-fault simulation time.  Every-256 keeps fresh data on
           disk (a full run of 10 k faults = ~40 dumps) but drops
           amortised dump cost to ~1 k simulated instrs/fault. */
        const uint64_t kDumpEvery = 256;
        uint64_t fc = m_fault_metrics.fault_count();
        if (fc > 0 && (fc % kDumpEvery) == 0) {
            std::string base = reportBase();
            if (instrumented) {
                m_fault_metrics.dump_csv(base + "_fault_phases.csv");
                m_fault_metrics.dump_histograms(base + "_fault_histograms.csv");
            }
            if (replay_enabled) {
                m_fault_replay.dump_stats(base + "_replay_stats.csv");
            }
            if (m_scheduler.is_enabled()) {
                m_scheduler.dump_stats(base + "_scheduler_stats.csv");
            }
            if (m_kernel_arena.enabled()) {
                m_kernel_arena.dump_csv(base + "_kernel_arena.csv");
            }
            if (m_fast_fault_registry.is_enabled()) {
                /* Mirror of the destructor's FastFault CSV so the data
                   survives stop-by-icount termination (which kills the
                   process before ~MimicOS runs). */
                std::ofstream f(base + "_fast_fault_profiles.csv");
                if (f.is_open()) {
                    f << "fp_id,mapping,access,page_size,pcp_class,pgt_level,vma_fresh,state,"
                         "samples,mean_cycles,stddev,cv,replay_count,"
                         "summary_len,captured_len,"
                         "resamples,drift_in_band,drift_out_band,drift_resets,drift_last_sample,"
                         "p50,p90,p99,pcp_hit_samples,pcp_miss_samples,pcp_unknown_samples\n";
                    for (const auto& kv : m_fast_fault_registry.table()) {
                        const FaultFingerprint& k = kv.first;
                        const FastFaultProfile& p = kv.second;
                        const char* st = "unknown";
                        switch (p.state) {
                            case FastFaultState::UNKNOWN:   st = "unknown"; break;
                            case FastFaultState::TRAINING:  st = "training"; break;
                            case FastFaultState::TRAINED:   st = "trained"; break;
                            case FastFaultState::ABANDONED: st = "abandoned"; break;
                        }
                        f << p.fp_id << ','
                          << static_cast<int>(k.mapping_type) << ','
                          << static_cast<int>(k.access_type) << ','
                          << static_cast<int>(k.target_page_size) << ','
                          << static_cast<int>(k.pcp_hit_or_miss) << ','
                          << static_cast<int>(k.pgtable_install_level) << ','
                          << static_cast<int>(k.vma_freshness) << ','
                          << st << ','
                          << p.sample_count << ','
                          << p.mean_cycles << ','
                          << p.stddev() << ','
                          << p.cv() << ','
                          << p.replay_count << ','
                          << p.memory_trace.size() << ','
                          << p.captured_accesses.size() << ','
                          << p.resample_count << ','
                          << p.drift_in_band << ','
                          << p.drift_out_band << ','
                          << p.drift_resets << ','
                          << p.drift_last_sample << ','
                          << p.quantile(0.50) << ','
                          << p.quantile(0.90) << ','
                          << p.quantile(0.99) << ','
                          << p.pcp_hit_samples << ','
                          << p.pcp_miss_samples << ','
                          << p.pcp_unknown_samples << '\n';
                    }
                }

                /* Welford time-series history (append-only).  One row
                   per (dump tick × profile), recording the running
                   Welford mean / stddev / cv plus state.  Lets us
                   reconstruct the convergence trajectory of every
                   profile across the run.  Dump rate is the same as
                   the periodic snapshot above (every 256 faults). */
                static bool welford_header_written = false;
                std::ofstream wf(base + "_welford_history.csv",
                                 welford_header_written
                                     ? std::ios::app
                                     : std::ios::trunc);
                if (wf.is_open()) {
                    if (!welford_header_written) {
                        wf << "fault_count,fp_id,pgt_level,vma_fresh,"
                              "pcp_class,state,samples,mean_cycles,"
                              "stddev,cv,replay_count\n";
                        welford_header_written = true;
                    }
                    for (const auto& kv : m_fast_fault_registry.table()) {
                        const FaultFingerprint& k = kv.first;
                        const FastFaultProfile& p = kv.second;
                        const char* st = "unknown";
                        switch (p.state) {
                            case FastFaultState::UNKNOWN:   st = "unknown"; break;
                            case FastFaultState::TRAINING:  st = "training"; break;
                            case FastFaultState::TRAINED:   st = "trained"; break;
                            case FastFaultState::ABANDONED: st = "abandoned"; break;
                        }
                        wf << fc << ','
                           << p.fp_id << ','
                           << static_cast<int>(k.pgtable_install_level) << ','
                           << static_cast<int>(k.vma_freshness) << ','
                           << static_cast<int>(k.pcp_hit_or_miss) << ','
                           << st << ','
                           << p.sample_count << ','
                           << p.mean_cycles << ','
                           << p.stddev() << ','
                           << p.cv() << ','
                           << p.replay_count << '\n';
                    }
                }
            }
        }
    }

}


// Define the static member variable
MimicOS* MimicOS::instance = nullptr;

/* KernelArena::charge_pfn — out-of-line so kernel_arena.h doesn't need the
   full PhysicalMemoryAllocator definition.  Bumps the simulated kernel
   reserve by exactly one PFN every time the arena crosses a 4 KB boundary. */
void KernelArena::charge_pfn() {
    if (allocator_) {
        UInt64 pfn = allocator_->handle_page_table_allocations(4096);
        (void)pfn;  /* the allocator already moved its kernel cursor */
    }
    pfns_consumed_++;
}