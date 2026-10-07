# Temporal PTE Prefetcher — Tuning Guide

## Overview

The Temporal PTE Prefetcher stores delta-encoded successor hints in PTE shadow
payloads.  On a TLB miss it reads the payload, decodes delta+confidence slots,
and issues page-table-transparent prefetch walks for predicted regions.

All knobs are in the config section:
```
[perf_model/mmu/tlb_prefetch/pq1/temporal_pte_prefetcher]
```
and can be overridden at runtime via `-g` flags.

---

## 1. Payload Codec (What Gets Stored in the PTE)

| Knob | Default | Description |
|------|---------|-------------|
| `num_offsets` | 4 | Number of delta+confidence slots per PTE |
| `offset_bits` | 18 | Bits per signed delta (two's complement, in region units) |
| `conf_bits` | 2 | Bits per confidence counter (0 = no confidence) |
| `base_bit` | 0 | Starting bit position within the 128-bit payload word |

**Hardware budget**: 8 PTEs/cache line x 10 spare bits = 80 bits.
Current default: 4 x (18+2) = 80 bits (exact fit).

**Trade-offs**:
- More slots (`num_offsets`) → capture more successors per page (top-1 accuracy
  is ~14%, top-4 reaches ~40%) but each slot gets fewer bits.
- Wider deltas (`offset_bits`) → larger reachable address range.  With 18-bit
  signed offsets and region_shift=3: +/-131K regions = +/-4GB.
- More confidence bits → finer-grained filtering but fewer bits for deltas.

**Constraint**: `num_offsets * (offset_bits + conf_bits) + base_bit <= 128`
(enforced by assertion; exceeding 80 bits is functionally correct but exceeds
the PTE cache line hardware budget).

---

## 2. Region Granularity

| Knob | Default | Description |
|------|---------|-------------|
| `page_shift` | 12 | log2(page size), always 12 for 4KB pages |
| `region_shift` | 3 | log2(pages per region) |

`region_shift` controls the prediction granularity:

| region_shift | Pages/region | Region size | Modeled walks/prediction | Fanout penalty |
|-------------|-------------|-------------|--------------------------|----------------|
| 3 | 8 | 32KB | 1 | 1/sqrt(8) = 0.354 |
| 4 | 16 | 64KB | 2 | 1/sqrt(16) = 0.250 |
| 5 | 32 | 128KB | 4 | 1/sqrt(32) = 0.177 |

**Trade-offs**:
- Larger regions → more pages prefetched per prediction (better coverage),
  but more modeled walks per prediction (higher latency), more cache pressure
  (more evictions), and lower fanout penalty (kills chaining faster).
- region_shift=3 is the sweet spot for most workloads: one modeled walk per
  prediction, matches PTE cache line granularity (8 PTEs = 1 cache line).

---

## 3. Confidence Gating (Depth 0)

| Knob | Default | Description |
|------|---------|-------------|
| `conf_threshold` | 2 | Minimum confidence to issue a depth-0 prefetch |
| `conf_init` | 1 | Initial confidence when a new delta is inserted |

A slot fires at depth 0 only if `conf >= conf_threshold`.

**Interaction with conf_init**:
- `conf_init=1, conf_threshold=2` → conservative: needs one confirming
  re-observation before firing.
- `conf_init=3, conf_threshold=2` → aggressive: fires on first insert
  (since 3 >= 2).  Good when transitions are mostly one-shot.
- `conf_init=0, conf_threshold=1` → ultra-conservative: needs two observations
  (insert at 0, bump to 1).

---

## 4. Chained (Recursive) Prefetching

| Knob | Default | Description |
|------|---------|-------------|
| `max_prefetch_depth` | 1 | Maximum chain depth (0 = no chaining) |
| `chain_edge_decay` | 0.75 | Per-edge multiplicative decay factor (0-1) |
| `chain_score_threshold` | 0.20 | Minimum path score to issue a chained prediction |
| `chain_conf_threshold` | 0 | Integer confidence floor for depth > 0 (0 = disabled) |

At depth > 0, two independent gates apply (both must pass):

1. **Integer confidence floor** (if `chain_conf_threshold > 0`):
   `conf >= chain_conf_threshold`, same semantics as depth-0's `conf_threshold`.
   Set to 0 to disable (default — only path-score gates).

2. **Path-score gating**:
   ```
   child_score = parent_score * (conf / max_conf) * edge_decay * fanout_penalty
   ```
   Child is issued only if `child_score >= chain_score_threshold`.

**How to disable all gating** (let `max_prefetch_depth` be the sole limiter):
```
chain_score_threshold = 0.0
chain_conf_threshold = 0
```

**How to match depth-0 policy at all depths**:
```
chain_conf_threshold = 2        # same as conf_threshold
chain_score_threshold = 0.0     # disable path-score gating
```

**Path-score example** (region_shift=3, edge_decay=0.85):
```
Depth 0→1: 1.0   x 1.0 x 0.85 x 0.354 = 0.301  (passes 0.05)
Depth 1→2: 0.301 x 1.0 x 0.85 x 0.354 = 0.091  (passes 0.05)
Depth 2→3: 0.091 x 1.0 x 0.85 x 0.354 = 0.027  (fails  0.05 → pruned)
```

**Tuning strategy**:
- Start with `max_prefetch_depth=3, chain_score_threshold=0.05, edge_decay=0.85`.
- If too aggressive (high evict-prefetch): raise `chain_score_threshold` or
  lower `edge_decay`.
- If chaining is dead (depth1_predictions=0): lower `chain_score_threshold`
  or raise `edge_decay` toward 1.0.
- Use `chain_conf_threshold` to enforce a hard minimum: e.g., set to 2 so
  only confirmed deltas chain, regardless of path score.

---

## 5. Confidence Policies

| Knob | Default | Description |
|------|---------|-------------|
| `confidence_policy` | competitive | Policy name (none/competitive/exponential/bump_only_strong) |
| `conf_bump_amount` | 1 | Increment on matching delta |
| `conf_decay_on_bump` | 1 | Decrement to OTHER slots on match (competitive only) |
| `conf_decay_on_miss` | 1 | Decrement to ALL slots on new delta (competitive only) |

**Policies**:
- `none` — bump matched slot, no decay.  Stale deltas persist until evicted
  by a new insert.
- `competitive` — bump matched slot, linearly decay others.  Creates pressure
  on stale slots, good for phase-changing workloads.
- `exponential` — bump matched slot, halve (>>1) others.  Faster adaptation.
- `bump_only_strong` — bump by 2, no decay.  Strong reinforcement for stable patterns.

**Tuning**: with 2-bit confidence (max=3), `competitive` with default params
means a single new delta observation decays all other slots by 1.  After 3
non-matching transitions, an old slot reaches conf=0 and becomes evictable.

---

## 6. PC-Conditioned Learning

| Knob | Default | Description |
|------|---------|-------------|
| `mode` | learn_pte_on_transitions | Learning mode |
| `pc_tag_bits` | 64 | Bits of PC tag for SRAM cache comparison |
| `pc_table_size` | 256 | Direct-mapped SRAM PC table entries (power of 2) |
| `reserve_global_slot` | true | Reserve last payload slot for non-PC delta |
| `virtualize_pc_table` | false | Enable radix-tree backing store behind SRAM cache |

**Modes**:
- `pte_only` — read-only: decode payloads, never write back.  No learning.
- `learn_pte_on_transitions` — global last-region tracker, no PC isolation.
- `pc_cond_learn` — per-PC last-region tracking via direct-mapped table.

**PC table sizing**: the SRAM cache must hold the working set of unique load PCs.
Check `pc_table_hits` vs `pc_table_misses` in sim.stats.  If miss rate is high,
either increase `pc_table_size` or enable `virtualize_pc_table`.

**Virtualized PC table**: evicted entries are written back to a 5-level radix
tree in emulated physical memory.  On cache miss, the backing store is probed
(costs 5 cache accesses).  Preserves stride state and last-VPN across evictions.

**Global slot** (`reserve_global_slot=true`): the last payload slot is reserved
for a PC-miss fallback.  When a PC misses the table, the transition delta goes
into slot N-1 only.  PC-hit transitions use slots 0..N-2.

---

## 7. Stride Detection + Direct Prefetch

| Knob | Default | Description |
|------|---------|-------------|
| `stride_direct_prefetch` | false | Master enable for stride prefetching |
| `stride_conf_threshold` | 3 | Consecutive matching deltas needed to confirm stride |
| `stride_direct_degree` | 4 | Pages to prefetch ahead along the stride |

**How it works**: a per-PC (or global) FSM tracks page-level deltas.  When the
same delta repeats `stride_conf_threshold` times, direct PTWTransparent walks
are issued for the next `stride_direct_degree` pages.

**Interaction with temporal deltas**: stride captures regular sequential access
(A, A+s, A+2s, ...) while temporal deltas capture irregular successors (A→B,
A→C).  They complement each other — stride prefetches fire from Phase 3 and
are tracked separately with `PrefetchSource::STRIDE` in timeliness stats.

**When to enable**: workloads with sequential page access patterns (e.g.,
streaming, graph traversal with locality).  The `rnd` trace (random access)
shows `stride_direct_issued=0` because there are no stable strides.

---

## 8. Hit-Path Behavior

| Knob | Default | Description |
|------|---------|-------------|
| `learn_on_hit` | false | Learn transitions on TLB hits (not just misses) |
| `prefetch_on_hit` | true | Issue prefetches on PQ hits (not just misses) |

- `prefetch_on_hit=true` — when a demand access hits a PQ-installed entry,
  read its payload and predict further.  This is the "chain forward on hit"
  behavior that keeps the prefetcher ahead of the access stream.
- `learn_on_hit=false` — only learn on misses to avoid learning transitions
  that were already predicted (self-reinforcing loops).

---

## 9. Replacement Policy

| Knob | Default | Description |
|------|---------|-------------|
| `replacement` | lowest_conf | Victim selection when all slots are full |

Options: `lowest_conf`, `random`, `lru` (falls back to lowest_conf — LRU
needs age bits not yet implemented).

---

## 10. Periodic Confidence Decay

| Knob | Default | Description |
|------|---------|-------------|
| `enable_decay` | false | Periodically halve all confidence counters |
| `decay_period` | 1000 | Learning updates between decay sweeps |

When enabled, every `decay_period` learning updates, all confidences in the
just-written payload are halved.  This prevents very old high-confidence
deltas from persisting indefinitely when access patterns change.

---

## Quick Reference: Recommended Starting Configs

**Conservative (low bandwidth, high accuracy)**:
```
conf_init=1, conf_threshold=2, max_prefetch_depth=0, confidence_policy=competitive
```

**Balanced (good for most workloads)**:
```
conf_init=3, conf_threshold=2, max_prefetch_depth=3, confidence_policy=none,
chain_edge_decay=0.85, chain_score_threshold=0.05,
virtualize_pc_table=true, pc_table_size=64, stride_direct_prefetch=true
```

**Aggressive (max coverage, higher cache pressure)**:
```
conf_init=3, conf_threshold=1, max_prefetch_depth=3,
chain_score_threshold=0.0, chain_conf_threshold=0
```

---

## Key Stats to Monitor

| Stat | What it tells you |
|------|-------------------|
| `queries_with_payload` / `queries` | Payload coverage (are PTEs being learned?) |
| `predictions_issued` | Total prefetches fired |
| `demand_hit_installed` | Useful prefetches (in TLB when demanded) |
| `demand_hit_installed_evicted` | Accurate but too early (evicted before demand) |
| `demand_hit_inflight` | Late prefetches (demand arrived before walk finished) |
| `demand_miss_not_prefetched` | True misses (never predicted) |
| `predictions_skipped_low_conf` | Slots rejected by conf_threshold |
| `predictions_skipped_tlb_residency` | Regions skipped (>50% already in TLB) |
| `L2.hits-prefetch` | Demand PTW L2 hits on prefetch-tagged lines |
| `L2.evict-prefetch` | Prefetch lines evicted unused (cache pollution) |
| `depth0_predictions` / `depth1_predictions` | Chaining activity by depth |
| `pc_table_hits` / `pc_table_misses` | PC table coverage |
| `stride_detected` | Stride confirmations |
| `learning_delta_out_of_range` | Deltas that don't fit in offset_bits |
