/**
 * @file mplru_bandit_controller.cc
 * @brief Implementation of NUCA MPKI-based bandit MPLRU controller
 * 
 * ============================================================================
 * ALGORITHM OVERVIEW
 * ============================================================================
 * 
 * This controller treats policy selection as a multi-armed bandit problem:
 * - 12 arms (policies 0-11: M0-M5 for metadata protection, D0-D5 for data protection)
 * - Reward = -MPKI (negative MPKI, so maximizing reward = minimizing MPKI)
 * - Goal: Minimize NUCA MPKI by selecting the best policy
 * 
 * POLICY ARMS:
 *   Arms 0-5 (M0-M5): Metadata protection (evict data preferentially)
 *   Arms 6-11 (D0-D5): Data protection (evict metadata preferentially)
 * 
 * EXPLOIT PHASE (default):
 *   - Use policy with highest Q-value
 *   - Update Q[current_level] with EMA of observed reward (-MPKI)
 * 
 * PROBE PHASE (periodic):
 *   - Every probe_period epochs, probe a different policy
 *   - Selection via UCB or neighbor heuristic
 *   - Probe for probe_length epochs
 *   - If MPKI rises above Q[best] * (1 + abort_threshold), ABORT
 *   - On abort: apply penalty_factor to Q[probe_level] and return to best
 *   - On completion: update Q[probe_level] and switch if better
 * 
 * WHY NUCA MPKI:
 *   - Direct measure of cache effectiveness
 *   - Captures both metadata and data caching performance
 *   - Lower MPKI = fewer memory accesses = better performance
 * 
 * WHY PROBE ABORT:
 *   - Prevents prolonged exploration of clearly bad policies
 *   - 15% threshold is conservative enough to allow minor variance
 *   - Penalty ensures we don't re-probe bad policies too aggressively
 * 
 * FORCED POLICY MODE:
 *   - Set force_policy=true and forced_policy_id=N in config
 *   - Bypasses all bandit logic and always returns policy N
 *   - Useful for experiments and debugging
 * 
 * ============================================================================
 */

#include "mplru_bandit_controller.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include "mimicos.h"
#include "sim_log.h"
#include "debug_config.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <random>

// ============================================================================
// PerCoreState constructor
// ============================================================================

MPLRUBanditController::PerCoreState::PerCoreState()
   // Configuration defaults
   : enabled(true)
   , epoch_instructions(100000)    // 100K instructions per epoch
   , beta(0.2f)                    // EMA smoothing factor
   , probe_period(10)              // Probe every 10 epochs
   , probe_length(5)               // 5-epoch probes (longer for stability)
   , abort_threshold(0.15f)        // Abort if MPKI > best * 1.15 (15% worse)
   , penalty_factor(0.99f)         // Penalize Q by 1% on abort (very light)
   , start_level(2)                // Start at L2 (middle level)
   , warmup_epochs(120)            // 120 epochs warmup (10 per arm for 12 arms)
   , ucb_c(1.0f)                   // UCB exploration coefficient
   , use_ucb(true)                 // Use UCB for probe selection
   // Force policy override
   , force_policy(false)           // Disabled by default
   , forced_policy_id(0)           // Default to arm 0 (vanilla LRU)
   // Runtime state
   , current_level(2)              // Start at middle level
   , best_level(2)
   , probing(false)
   , probe_level(0)
   , epochs_since_probe(0)
   , probe_epochs_remaining(0)
   , engaged(true)                 // Start engaged since level > 0
   , in_warmup(true)               // Start in warmup phase
   , warmup_level(0)               // Start warmup at L0
   , warmup_epochs_remaining(120)  // Full warmup duration for 12 arms
   , warmup_order_initialized(false) // Will be shuffled on first epoch
   // MPKI tracking
   , last_epoch_instructions(0)
   , last_instructions(0)
   , last_nuca_misses(0)
   , mpki_window(0.0f)
   // Stats
   , total_epochs(0)
   , probes_started(0)
   , probes_completed(0)
   , probes_aborted(0)
   , level_switches(0)
   // Stats snapshots
   , current_level_stat(0)
   , current_best_level_stat(0)
   , current_probing_stat(0)
   , mpki_x1000(0)
   , force_policy_stat(0)
   , forced_policy_id_stat(0)
{
   // Initialize Q-values with optimistic initialization
   // Higher initial values encourage exploration of all levels
   // Note: Q represents -MPKI, so 0 is optimistic (assumes zero misses)
   for (int i = 0; i < NUM_LEVELS; i++) {
      Q[i] = 0.0f;          // Optimistic: assume all policies are good (0 MPKI)
      Q_x1000[i] = 0;
      epochs_at_level[i] = 0;
      n_samples[i] = 0;     // No samples yet (for UCB)
      warmup_order[i] = i;  // Default sequential order (will be shuffled)
   }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

MPLRUBanditController::MPLRUBanditController()
   : m_initialized(false)
{
}

MPLRUBanditController::~MPLRUBanditController()
{
   cleanup();
}

// ============================================================================
// Initialization / Cleanup
// ============================================================================

void MPLRUBanditController::initialize(UInt32 num_cores)
{
   std::lock_guard<std::mutex> lock(m_mutex);
   
   if (m_initialized) {
      return;
   }
   
   m_per_core_state.resize(num_cores);
   
   // Create per-core loggers
   m_per_core_log.resize(num_cores, nullptr);
   for (UInt32 i = 0; i < num_cores; i++) {
      m_per_core_log[i] = new SimLog("MPLRU-Bandit", i, DEBUG_MPLRU);
   }
   
   if (num_cores > 0 && m_per_core_log[0]) {
      m_per_core_log[0]->info("Initialized for ", num_cores, " cores");
   }
   
   m_initialized = true;
}

void MPLRUBanditController::cleanup()
{
   std::lock_guard<std::mutex> lock(m_mutex);
   
   if (!m_initialized) {
      return;
   }
   
   // Print final stats
   for (size_t i = 0; i < m_per_core_state.size(); i++) {
      const PerCoreState& state = m_per_core_state[i];
      if (state.enabled && state.total_epochs > 0) {
         if (m_per_core_log[i]) {
            m_per_core_log[i]->info("═══════════════════════════════════════════");
            m_per_core_log[i]->info("FINAL BANDIT STATS");
            m_per_core_log[i]->info("  total_epochs=", state.total_epochs);
            m_per_core_log[i]->info("  probes_started=", state.probes_started);
            m_per_core_log[i]->info("  probes_completed=", state.probes_completed);
            m_per_core_log[i]->info("  probes_aborted=", state.probes_aborted);
            m_per_core_log[i]->info("  level_switches=", state.level_switches);
            m_per_core_log[i]->info("  final_level=", state.current_level);
            m_per_core_log[i]->info("  best_level=", state.best_level);
            for (int l = 0; l < NUM_LEVELS; l++) {
               m_per_core_log[i]->info("  Q[", l, "]=", state.Q[l]);
            }
            m_per_core_log[i]->info("═══════════════════════════════════════════");
         }
      }
   }
   
   // Cleanup loggers
   for (size_t i = 0; i < m_per_core_log.size(); i++) {
      delete m_per_core_log[i];
   }
   m_per_core_log.clear();
   m_per_core_state.clear();
   m_initialized = false;
}

// ============================================================================
// Configuration
// ============================================================================

void MPLRUBanditController::loadConfig(core_id_t core_id, const String& cfgname)
{
   if (!m_initialized || core_id >= (core_id_t)m_per_core_state.size()) {
      return;
   }
   
   PerCoreState& state = m_per_core_state[core_id];
   
   // Every knob is optional with a built-in default.  A missing key has to
   // be tested for: the configuration layer exits on one rather than
   // throwing, so a try/catch around the lookup (as this once was) never
   // caught anything and the default was unreachable -- a config that set
   // only the knobs it cared about (reservethp_mplru_bandit.cfg) could not
   // start.
   const String base = cfgname + "/mplru/controller/";
   auto getBool = [&](const String& key, bool def) -> bool {
      return Sim()->getCfg()->hasKey(base + key) ? Sim()->getCfg()->getBoolArray(base + key, core_id) : def;
   };
   auto getInt = [&](const String& key, int def) -> int {
      return Sim()->getCfg()->hasKey(base + key) ? (int)Sim()->getCfg()->getIntArray(base + key, core_id) : def;
   };
   auto getFloat = [&](const String& key, float def) -> float {
      return Sim()->getCfg()->hasKey(base + key) ? (float)Sim()->getCfg()->getFloatArray(base + key, core_id) : def;
   };
   
   state.enabled = getBool("enabled", true);
   state.epoch_instructions = getInt("epoch_instructions", 100000);
   state.beta = getFloat("beta", 0.2f);
   state.probe_period = getInt("probe_period", 10);
   state.probe_length = getInt("probe_length", 5);
   state.abort_threshold = getFloat("abort_threshold", 0.15f);
   state.penalty_factor = getFloat("penalty_factor", 0.99f);
   state.start_level = getInt("start_level", 2);
   state.warmup_epochs = getInt("warmup_epochs", 120);  // 120 for 12 arms
   state.ucb_c = getFloat("ucb_c", 1.0f);
   state.use_ucb = getBool("use_ucb", true);
   
   // Force policy override config
   state.force_policy = getBool("force_policy", false);
   state.forced_policy_id = getInt("forced_policy_id", 0);
   state.forced_policy_id = std::clamp(state.forced_policy_id, 0, NUM_LEVELS - 1);
   
   // Apply start_level to initial state (unless force_policy is active)
   if (state.force_policy) {
      state.current_level = state.forced_policy_id;
      state.best_level = state.forced_policy_id;
      state.engaged = (state.forced_policy_id != 0 && state.forced_policy_id != 6);
      state.in_warmup = false;  // Skip warmup when forcing policy
      state.warmup_epochs_remaining = 0;
   } else {
      state.current_level = state.start_level;
      state.best_level = state.start_level;
      state.engaged = (state.start_level > 0);
      state.warmup_epochs_remaining = state.warmup_epochs;
      state.in_warmup = (state.warmup_epochs > 0);
   }
   
   if (m_per_core_log[core_id]) {
      m_per_core_log[core_id]->info("Config: enabled=", state.enabled,
                " epoch_instructions=", state.epoch_instructions,
                " beta=", state.beta,
                " probe_period=", state.probe_period,
                " probe_length=", state.probe_length,
                " abort_threshold=", state.abort_threshold,
                " start_level=", state.start_level,
                " warmup_epochs=", state.warmup_epochs,
                " use_ucb=", state.use_ucb);
      m_per_core_log[core_id]->info("Config: force_policy=", state.force_policy,
                " forced_policy_id=", state.forced_policy_id);
   }
   
   // Register stats after loading config
   registerStats(core_id);
}

// ============================================================================
// Statistics Registration
// ============================================================================

void MPLRUBanditController::registerStats(core_id_t core_id)
{
   if (!m_initialized || core_id >= (core_id_t)m_per_core_state.size()) {
      return;
   }
   
   PerCoreState& state = m_per_core_state[core_id];
   String name = "mplru-bandit";
   
   // Epoch/probe tracking
   registerStatsMetric(name, core_id, "total-epochs", &state.total_epochs);
   registerStatsMetric(name, core_id, "probes-started", &state.probes_started);
   registerStatsMetric(name, core_id, "probes-completed", &state.probes_completed);
   registerStatsMetric(name, core_id, "probes-aborted", &state.probes_aborted);
   registerStatsMetric(name, core_id, "level-switches", &state.level_switches);
   
   // Per-level epoch counters (12 arms: M0-M5, D0-D5)
   registerStatsMetric(name, core_id, "epochs-at-level-0", &state.epochs_at_level[0]);   // M0
   registerStatsMetric(name, core_id, "epochs-at-level-1", &state.epochs_at_level[1]);   // M1
   registerStatsMetric(name, core_id, "epochs-at-level-2", &state.epochs_at_level[2]);   // M2
   registerStatsMetric(name, core_id, "epochs-at-level-3", &state.epochs_at_level[3]);   // M3
   registerStatsMetric(name, core_id, "epochs-at-level-4", &state.epochs_at_level[4]);   // M4
   registerStatsMetric(name, core_id, "epochs-at-level-5", &state.epochs_at_level[5]);   // M5
   registerStatsMetric(name, core_id, "epochs-at-level-6", &state.epochs_at_level[6]);   // D0
   registerStatsMetric(name, core_id, "epochs-at-level-7", &state.epochs_at_level[7]);   // D1
   registerStatsMetric(name, core_id, "epochs-at-level-8", &state.epochs_at_level[8]);   // D2
   registerStatsMetric(name, core_id, "epochs-at-level-9", &state.epochs_at_level[9]);   // D3
   registerStatsMetric(name, core_id, "epochs-at-level-10", &state.epochs_at_level[10]); // D4
   registerStatsMetric(name, core_id, "epochs-at-level-11", &state.epochs_at_level[11]); // D5
   
   // Current state snapshots
   registerStatsMetric(name, core_id, "current-level", &state.current_level_stat);
   registerStatsMetric(name, core_id, "best-level", &state.current_best_level_stat);
   registerStatsMetric(name, core_id, "probing", &state.current_probing_stat);
   registerStatsMetric(name, core_id, "mpki-x1000", &state.mpki_x1000);
   
   // Force policy stats
   registerStatsMetric(name, core_id, "force-policy", &state.force_policy_stat);
   registerStatsMetric(name, core_id, "forced-policy-id", &state.forced_policy_id_stat);
   
   // Q-values (x1000) - 12 arms
   registerStatsMetric(name, core_id, "Q0-x1000", &state.Q_x1000[0]);
   registerStatsMetric(name, core_id, "Q1-x1000", &state.Q_x1000[1]);
   registerStatsMetric(name, core_id, "Q2-x1000", &state.Q_x1000[2]);
   registerStatsMetric(name, core_id, "Q3-x1000", &state.Q_x1000[3]);
   registerStatsMetric(name, core_id, "Q4-x1000", &state.Q_x1000[4]);
   registerStatsMetric(name, core_id, "Q5-x1000", &state.Q_x1000[5]);
   registerStatsMetric(name, core_id, "Q6-x1000", &state.Q_x1000[6]);
   registerStatsMetric(name, core_id, "Q7-x1000", &state.Q_x1000[7]);
   registerStatsMetric(name, core_id, "Q8-x1000", &state.Q_x1000[8]);
   registerStatsMetric(name, core_id, "Q9-x1000", &state.Q_x1000[9]);
   registerStatsMetric(name, core_id, "Q10-x1000", &state.Q_x1000[10]);
   registerStatsMetric(name, core_id, "Q11-x1000", &state.Q_x1000[11]);
   
   if (m_per_core_log[core_id]) {
      m_per_core_log[core_id]->info("Registered stats metrics");
   }
}

// ============================================================================
// Stats Snapshot Update
// ============================================================================

void MPLRUBanditController::updateStatsSnapshots(PerCoreState& state)
{
   state.current_level_stat = (UInt64)state.current_level;
   state.current_best_level_stat = (UInt64)state.best_level;
   state.current_probing_stat = state.probing ? 1 : 0;
   state.mpki_x1000 = (UInt64)(state.mpki_window * 1000.0f);
   state.force_policy_stat = state.force_policy ? 1 : 0;
   state.forced_policy_id_stat = (UInt64)state.forced_policy_id;
   
   for (int i = 0; i < NUM_LEVELS; i++) {
      // Q is -MPKI, so we store the absolute value * 1000 for stats
      // (negative Q means higher MPKI which is worse)
      state.Q_x1000[i] = (UInt64)(std::abs(state.Q[i]) * 1000.0f);
   }
}

// ============================================================================
// Calculate UCB Score
// ============================================================================

float MPLRUBanditController::calculateUCB(const PerCoreState& state, int level) const
{
   if (level < 0 || level >= NUM_LEVELS) return 0.0f;
   
   // UCB1: Q[level] + c * sqrt(ln(N) / n[level])
   // Where N = total samples, n[level] = samples for this level
   
   UInt64 total_samples = 0;
   for (int i = 0; i < NUM_LEVELS; i++) {
      total_samples += state.n_samples[i];
   }
   
   // If no samples for this level, return infinity (must explore)
   if (state.n_samples[level] == 0) {
      return 1000.0f;  // Very high value to force exploration
   }
   
   // If no total samples yet, just return Q-value
   if (total_samples == 0) {
      return state.Q[level];
   }
   
   float exploration_bonus = state.ucb_c * std::sqrt(
      std::log((float)total_samples) / (float)state.n_samples[level]
   );
   
   return state.Q[level] + exploration_bonus;
}

// ============================================================================
// Choose Probe Level
// ============================================================================

int MPLRUBanditController::chooseProbeLevel(const PerCoreState& state)
{
   if (state.use_ucb) {
      // UCB-based selection: choose level with highest UCB score
      // (excluding current level to ensure exploration)
      int best_probe = -1;
      float best_ucb = -1.0f;
      
      for (int level = 0; level < NUM_LEVELS; level++) {
         if (level == state.current_level) continue;  // Skip current level
         
         float ucb = calculateUCB(state, level);
         if (ucb > best_ucb) {
            best_ucb = ucb;
            best_probe = level;
         }
      }
      
      return (best_probe >= 0) ? best_probe : state.current_level;
   } else {
      // Original neighbor-based selection
      int lower = state.current_level - 1;
      int upper = state.current_level + 1;
      
      if (lower < 0) return upper;
      if (upper >= NUM_LEVELS) return lower;
      
      // Choose neighbor with lower Q-value (more potential upside)
      return (state.Q[lower] <= state.Q[upper]) ? lower : upper;
   }
}

// ============================================================================
// Epoch Processing (main bandit logic)
// ============================================================================

void MPLRUBanditController::processEpoch(core_id_t core_id)
{
   if (!m_initialized || core_id >= (core_id_t)m_per_core_state.size()) {
      return;
   }
   
   PerCoreState& state = m_per_core_state[core_id];
   
   if (!state.enabled) {
      return;
   }
   
   SimLog* log = (core_id < (core_id_t)m_per_core_log.size()) ? m_per_core_log[core_id] : nullptr;
   
   // ========================================
   // STEP 0: Handle forced policy mode
   // ========================================
   if (state.force_policy) {
      // When forcing policy, still track MPKI for stats but skip all learning
      MimicOS* mimicos = Sim()->getMimicOS();
      if (mimicos && mimicos->isPerCoreStatsInitialized()) {
         const PerCoreStats& stats = mimicos->getPerCoreStats(core_id);
         UInt64 delta_instructions = stats.instructions_executed - state.last_instructions;
         UInt64 delta_nuca_misses = stats.nuca_misses - state.last_nuca_misses;
         
         state.last_instructions = stats.instructions_executed;
         state.last_nuca_misses = stats.nuca_misses;
         
         if (delta_instructions > 0) {
            state.mpki_window = 1000.0f * (float)delta_nuca_misses / (float)delta_instructions;
         }
      }
      
      state.current_level = state.forced_policy_id;
      state.best_level = state.forced_policy_id;
      state.probing = false;
      state.engaged = (state.forced_policy_id != 0 && state.forced_policy_id != 6);
      state.total_epochs++;
      int level = std::clamp(state.forced_policy_id, 0, NUM_LEVELS - 1);
      state.epochs_at_level[level]++;
      updateStatsSnapshots(state);
      
      if (log) {
         log->trace("FORCED POLICY: policy_id=", state.forced_policy_id, 
                    " MPKI=", state.mpki_window);
      }
      return;
   }
   
   // ========================================
   // STEP 1: Pull stats from MimicOS
   // ========================================
   MimicOS* mimicos = Sim()->getMimicOS();
   if (!mimicos || !mimicos->isPerCoreStatsInitialized()) {
      return;
   }
   const PerCoreStats& stats = mimicos->getPerCoreStats(core_id);
   
   UInt64 current_instructions = stats.instructions_executed;
   UInt64 current_nuca_misses = stats.nuca_misses;
   
   // Compute MPKI for this epoch: (delta_misses / delta_instructions) * 1000
   UInt64 delta_instructions = current_instructions - state.last_instructions;
   UInt64 delta_nuca_misses = current_nuca_misses - state.last_nuca_misses;
   
   // Update baselines
   state.last_instructions = current_instructions;
   state.last_nuca_misses = current_nuca_misses;
   
   // Skip if epoch has no instructions (avoid division by zero)
   if (delta_instructions < 100) {
      if (log) log->trace("Epoch skipped (too short): delta_instructions=", delta_instructions);
      return;
   }
   
   // Compute MPKI (misses per kilo-instruction)
   state.mpki_window = 1000.0f * (float)delta_nuca_misses / (float)delta_instructions;
   
   // Compute reward: negative MPKI (maximize reward = minimize MPKI)
   float reward = -state.mpki_window;
   
   if (log) {
      log->trace("───────────────────────────────────────────");
      log->trace("EPOCH: dI=", delta_instructions, " dMiss=", delta_nuca_misses, 
                 " MPKI=", state.mpki_window, " reward=", reward);
      log->trace("  current_level=", state.current_level, 
                 " best_level=", state.best_level,
                 " probing=", state.probing ? "YES" : "no");
   }
   
   // ========================================
   // STEP 2: Track time at current level
   // ========================================
   state.total_epochs++;
   int level = std::clamp(state.current_level, 0, NUM_LEVELS - 1);
   state.epochs_at_level[level]++;
   state.epochs_since_probe++;
   
   // ========================================
   // STEP 3: Update Q-value for current level (EMA)
   // ========================================
   // Q[L] = β * reward + (1-β) * Q[L]
   // reward = -MPKI, so higher Q = lower MPKI = better
   int active_level = state.probing ? state.probe_level : state.current_level;
   state.Q[active_level] = state.beta * reward + (1.0f - state.beta) * state.Q[active_level];
   state.n_samples[active_level]++;  // Track sample count for UCB
   
   // ========================================
   // STEP 3.1: Merge M0==D0 (they're equivalent policies - vanilla LRU)
   // ========================================
   // After any update to level 0 or 6, sync their Q-values and sample counts
   if (active_level == 0 || active_level == 6) {
      // Average the Q-values and sync
      float avg_q = (state.Q[0] + state.Q[6]) / 2.0f;
      state.Q[0] = avg_q;
      state.Q[6] = avg_q;
      // Also sync sample counts
      UInt64 total_samples = state.n_samples[0] + state.n_samples[6];
      state.n_samples[0] = total_samples;
      state.n_samples[6] = total_samples;
   }
   
   if (log) {
      log->debug("  Updated Q[", active_level, "]=", state.Q[active_level],
                 " (MPKI≈", -state.Q[active_level], ")",
                 " n_samples=", state.n_samples[active_level]);
   }
   
   // ========================================
   // STEP 3.5: WARMUP PHASE - Cycle through all levels (randomized order)
   // ========================================
   if (state.in_warmup) {
      // Shuffle warmup order on first epoch
      if (!state.warmup_order_initialized) {
         // Use core_id as seed for reproducibility per-core but different across cores
         std::mt19937 rng(core_id + 42);
         std::shuffle(state.warmup_order, state.warmup_order + NUM_LEVELS, rng);
         state.warmup_order_initialized = true;
         if (log) {
            log->info("WARMUP: shuffled order = [",
                      state.warmup_order[0], ",", state.warmup_order[1], ",",
                      state.warmup_order[2], ",", state.warmup_order[3], ",",
                      state.warmup_order[4], ",", state.warmup_order[5], ",",
                      state.warmup_order[6], ",", state.warmup_order[7], ",",
                      state.warmup_order[8], ",", state.warmup_order[9], ",",
                      state.warmup_order[10], ",", state.warmup_order[11], "]");
         }
      }
      
      state.warmup_epochs_remaining--;
      
      // Calculate how many epochs per level during warmup
      int epochs_per_level = state.warmup_epochs / NUM_LEVELS;
      if (epochs_per_level < 1) epochs_per_level = 1;
      
      // Determine which slot we should be at based on warmup progress
      int warmup_elapsed = state.warmup_epochs - state.warmup_epochs_remaining;
      int warmup_slot = (warmup_elapsed / epochs_per_level) % NUM_LEVELS;
      
      // Use randomized warmup_order instead of sequential
      int target_warmup_level = state.warmup_order[warmup_slot];
      
      if (state.warmup_level != warmup_slot) {
         state.warmup_level = warmup_slot;
         state.current_level = target_warmup_level;
         state.level_switches++;
         if (log) {
            log->info("WARMUP: rotating to level=", target_warmup_level,
                      " (slot ", warmup_slot, ", epoch ", warmup_elapsed, "/", state.warmup_epochs, ")");
         }
      }
      
      // Check if warmup is complete
      if (state.warmup_epochs_remaining <= 0) {
         state.in_warmup = false;
         // After warmup, start at the current best level
         state.current_level = state.best_level;
         if (log) {
            log->info("WARMUP COMPLETE: starting exploitation at best_level=", state.best_level);
            for (int i = 0; i < NUM_LEVELS; i++) {
               log->info("  Q[", i, "]=", state.Q[i], " (MPKI≈", -state.Q[i], ") samples=", state.n_samples[i]);
            }
         }
      }
      
      // During warmup, skip normal probing logic
      // Engaged if using any non-vanilla policy (not 0 or 6)
      state.engaged = (state.current_level != 0 && state.current_level != 6);
      updateStatsSnapshots(state);
      return;
   }
   
   // ========================================
   // STEP 4: Update best_level (argmax of Q)
   // ========================================
   state.best_level = 0;
   float best_q = state.Q[0];
   for (int i = 1; i < NUM_LEVELS; i++) {
      if (state.Q[i] > best_q) {
         best_q = state.Q[i];
         state.best_level = i;
      }
   }
   
   // ========================================
   // STEP 5: Handle probing
   // ========================================
   if (state.probing) {
      // Check abort condition: MPKI > best_expected_MPKI * (1 + abort_threshold)
      // best_expected_MPKI = -Q[best_level]
      // If current MPKI exceeds ceiling, abort
      float best_mpki_est = -state.Q[state.best_level];
      float abort_ceiling = best_mpki_est * (1.0f + state.abort_threshold);
      
      // Also handle case where best_mpki_est is very small or negative
      // In that case, use absolute threshold
      if (best_mpki_est < 0.1f) {
         abort_ceiling = 0.1f * (1.0f + state.abort_threshold);
      }
      
      if (state.mpki_window > abort_ceiling) {
         // ABORT: Probe is clearly worse, bail out
         state.probes_aborted++;
         // Apply penalty to Q-value (make it less attractive)
         state.Q[state.probe_level] *= state.penalty_factor;
         
         // Merge M0==D0 if penalty was applied to either
         if (state.probe_level == 0 || state.probe_level == 6) {
            float avg_q = (state.Q[0] + state.Q[6]) / 2.0f;
            state.Q[0] = avg_q;
            state.Q[6] = avg_q;
         }
         
         state.probing = false;
         state.probe_epochs_remaining = 0;
         
         if (log) {
            log->info("PROBE ABORT: MPKI=", state.mpki_window, " > ceiling=", abort_ceiling,
                      " → penalized Q[", state.probe_level, "]=", state.Q[state.probe_level]);
         }
         
         // Return to best known level
         if (state.current_level != state.best_level) {
            state.level_switches++;
            state.current_level = state.best_level;
            if (log) log->info("  Returning to best_level=", state.best_level);
         }
      } else {
         // Continue probing
         state.probe_epochs_remaining--;
         
         if (state.probe_epochs_remaining <= 0) {
            // Probe completed
            state.probes_completed++;
            state.probing = false;
            
            if (log) {
               log->info("PROBE COMPLETE: Q[", state.probe_level, "]=", state.Q[state.probe_level],
                         " (MPKI≈", -state.Q[state.probe_level], ")");
            }
            
            // Switch to probe level if it became best
            if (state.probe_level == state.best_level && 
                state.current_level != state.best_level) {
               state.level_switches++;
               state.current_level = state.best_level;
               if (log) log->info("  Switching to probe_level=", state.probe_level);
            }
         }
      }
   }
   
   // ========================================
   // STEP 6: Start new probe if due
   // ========================================
   if (!state.probing && state.epochs_since_probe >= state.probe_period) {
      state.probe_level = chooseProbeLevel(state);
      state.probing = true;
      state.probe_epochs_remaining = state.probe_length;
      state.epochs_since_probe = 0;
      state.probes_started++;
      
      if (log) {
         log->info("PROBE START: level=", state.probe_level, 
                   " (from current=", state.current_level, ")");
      }
   }
   
   // ========================================
   // STEP 7: Exploit if not probing
   // ========================================
   if (!state.probing) {
      // Switch to best level if not already there
      if (state.current_level != state.best_level) {
         state.level_switches++;
         state.current_level = state.best_level;
         if (log) log->debug("EXPLOIT: switching to best_level=", state.best_level);
      }
   }
   
   // ========================================
   // STEP 8: Update engaged status
   // ========================================
   // Engaged if using any protection policy (not vanilla LRU: 0 or 6)
   state.engaged = (state.current_level != 0 && state.current_level != 6);
   
   // ========================================
   // STEP 9: Update stats snapshots
   // ========================================
   updateStatsSnapshots(state);
   
   if (log) {
      log->debug("  After epoch: level=", state.current_level, 
                 " best=", state.best_level, " engaged=", state.engaged,
                 " MPKI=", state.mpki_window);
   }
}

// ============================================================================
// Try Epoch Processing
// ============================================================================

void MPLRUBanditController::tryProcessEpoch(core_id_t core_id)
{
   if (!m_initialized || core_id >= (core_id_t)m_per_core_state.size()) {
      return;
   }
   
   PerCoreState& state = m_per_core_state[core_id];
   
   if (!state.enabled) {
      return;
   }
   
   // Get current instruction count from MimicOS
   MimicOS* mimicos = Sim()->getMimicOS();
   if (!mimicos || !mimicos->isPerCoreStatsInitialized()) {
      return;
   }
   
   const PerCoreStats& stats = mimicos->getPerCoreStats(core_id);
   UInt64 current_instructions = stats.instructions_executed;
   
   // Check if enough instructions have passed since last epoch
   if (current_instructions - state.last_epoch_instructions >= state.epoch_instructions) {
      SimLog* log = (core_id < (core_id_t)m_per_core_log.size()) ? m_per_core_log[core_id] : nullptr;
      if (log) log->trace("Epoch triggered: instructions=", current_instructions);
      state.last_epoch_instructions = current_instructions;
      processEpoch(core_id);
   }
}

// ============================================================================
// Getters
// ============================================================================

int MPLRUBanditController::getMetaLevel(core_id_t core_id) const
{
   if (!m_initialized || core_id >= (core_id_t)m_per_core_state.size()) {
      return 0;
   }
   
   const PerCoreState& state = m_per_core_state[core_id];
   
   // Compile-time forced policy (uncomment to enable)
   // #define MPLRU_FORCE_POLICY_ID 9
   #ifdef MPLRU_FORCE_POLICY_ID
   return MPLRU_FORCE_POLICY_ID;
   #endif
   
   // Config-based forced policy
   if (state.force_policy) {
      return state.forced_policy_id;
   }
   
   // During probing, return the probe level; otherwise current level
   return state.probing ? state.probe_level : state.current_level;
}

bool MPLRUBanditController::isEngaged(core_id_t core_id) const
{
   if (!m_initialized || core_id >= (core_id_t)m_per_core_state.size()) {
      return false;
   }
   return m_per_core_state[core_id].engaged;
}
