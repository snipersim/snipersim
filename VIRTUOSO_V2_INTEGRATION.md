# Integrating Virtuoso v2 (TRAIL, Revelator, Victima, MimicOS v2) into Sniper

Working plan for the `dev-mimicos-v2` branch.

This builds on `VIRTUOSO_TO_SNIPER_MIGRATION_PLAN.md`, which did the hard research:
the three-way file classification, the per-file hunk ranges, and the findings in its
§1. That research is not repeated here. What this document adds is the **order**, an
organising principle the source plan does not use, and a set of corrections found by
re-running its analysis against this branch rather than against `56505e4`.

Source of truth for the port is Virtuoso `09c00ea` (main, 2026-08-03). Verified: the
`trail-artifact-release` and `revelator-artifact-release` branches carry byte-identical
code (`simulator/sniper` is tree `dabdd4ef` on all three), so they matter only as the
record of which experiment configurations define each paper's results.

---

## 1. The organising principle: orthogonal first

The source plan is ordered by dependency (A infrastructure → B TRAIL → C Revelator →
D OS models → E MimicOS v2). That order front-loads the most invasive work in the tree:
Phase A rewrites `cache_cntlr`, `mmu_base`, `mmu.cc` and `CacheBlockInfo`, which is
every memory-hierarchy hot path Sniper has, before anything new can be run.

This plan orders by **blast radius** instead. A change is *orthogonal* when no existing
configuration can reach it. Concretely, three tiers:

| Tier | Definition | How it is proven |
|---|---|---|
| **T0 — inert** | New files only. Nothing selects them; nothing links against them. | Existing configs produce **byte-identical** `sim.stats`. |
| **T1 — config-gated** | New files plus factory `else if` branches and default-off config keys. Reachable only by naming the feature. | Byte-identical with the feature off; the feature runs when named. |
| **T2 — invasive** | Edits to code that executes on existing paths. | Numbers may legitimately move. Requires a stated, reviewed reason per step. |

The payoff is that **most of the headline value is T0/T1**. Revelator, Victima, the
Linux daemon models and the whole userspace MimicOS substrate are new subsystems hanging
off factories. TRAIL is the outlier: it genuinely requires T2 surgery in the TLB and
cache path, which is why it is deliberately late here rather than early.

Rule for the branch: **a T2 step never shares a commit with a T0/T1 step.** When a
number moves, exactly one commit can be responsible.

---

## 2. Corrections to the source plan

Each was found by re-running the source plan's own analysis against this branch. These
are corrections to *that* document; the numbers it reports were correct for `56505e4`.

### 2.1 Its anchors have drifted — 10 files reclassified

`dev-build` is 105 commits and 160 changed files beyond `56505e4`. Re-running the
three-way classification moves ten files:

| Was | Now | File | Affects |
|---|---|---|---|
| CLEAN-PATCH | CONFLICT | `sift/sift_writer.cc`, `sift/zfstream.cc` | **A4** |
| CLEAN-PATCH | CONFLICT | `sift/recorder/sift_recorder.cc`, `syscall_modeling.cc` | **E3** |
| CLEAN-PATCH | CONFLICT | `sift/recorder/recorder_control.cc` | **E1** |
| CLEAN-PATCH | CONFLICT | `run-sniper` | tooling |
| CONFLICT | NEVER-PORTED | `common/Makefile.common`, `mimicos/Makefile` | §2.1 |
| CLEAN-PATCH | NEVER-PORTED | `sift/recorder/makefile.pin.rules` | §2.1 |

A4 deserves attention: this branch already hardened `zfstream.cc` (short-read detection,
sticky `read_failed`) and `sift_writer.cc` for its own reasons. A4 must be re-derived
against what is there, not applied — and may be partly redundant.

### 2.2 The build-system ground rules are obsolete

Source §2.1 says "no hunks from `Makefile`, `common/Makefile.common`, `mimicos/Makefile`
or `sift/recorder/makefile.pin.rules`". **All four files are gone** — this tree builds
with CMake. There is nothing to refuse hunks into; the equivalent concerns become CMake
options, and `-std=c++17` is already the project default, so source §2.1's C++ standard
caveat for E3 does not apply.

Source §1.5's hazard does survive, and matters: `CMakeLists.txt` uses
`file(GLOB_RECURSE CARBON_SOURCES CONFIGURE_DEPENDS "common/*.cc")`. Any `.cc` dropped
under `common/` still compiles immediately, so the duplicate-allocator collision the
source plan warns about in §G6 is still live.

### 2.3 Phase C is missing a prerequisite: `PDDM/misc/`

The Revelator family needs `bloom_filter` and `CityHash64`:

- `std::vector<bloom_filter *>` is a **member** of `revelator.h`,
  `revelator_open_addressing_engine.h` and `revelator_thp_engine.h`.
- `CityHash64` is called in `revelator.cc`, `revelator_thp_engine.cc`,
  `revelator_open_addressing_engine.cc`, `numa_revelator.{h,cc}`.

Both live in Virtuoso at `PDDM/misc/{bloom_filter.h, city.h, city.cc}`. They are absent
from Sniper and **not in the delta** — they are pre-existing Virtuoso files the March
port never took, so they appear nowhere in the source plan. Phase C does not compile
without them. They are pure additions (step **O1** below).

### 2.4 Step C2 is dead code — drop it

The source plan's C2 ports a `cache_cntlr` hook for Revelator. In Virtuoso `09c00ea`:

- `revelatordoPrefetch` is **declared** at `cache_cntlr.h:383` and never defined and
  never called, anywhere in the tree.
- `revelator_hits` / `revelator_total_preds` are members at `cache_cntlr.h:284` that are
  never registered as statistics and never written.

Porting C2 would add an undefined method declaration and two dead members to Sniper's
cache controller. **Drop C2 entirely.** Its consequences:

- Revelator needs *no* edit to `cache_cntlr.{h,cc}`, which moves the whole feature from
  T2 to T1 and is the main reason it is scheduled early here.
- The source plan's C4 check "`revelator_hits`/`revelator_total_preds` non-zero" is
  unachievable and must be replaced. Use the `mmu_spec` speculation counters instead
  (`mmu_spec.cc` tracks correct/misspeculated predictions directly).

### 2.5 `revelator.h` carries five stale includes, not two

Source §C4 says to drop `mmu_midgard.h` and `mmu_utopia.h`. `revelator.h` also includes
`mmu_pomtlb.h`, `mmu_range.h` and `spectlb.h`, none of which exist in Sniper. All five
are verified unused by symbol (the only `SpecTLB` occurrences are doxygen comments
copied from another file). Drop all five.

### 2.6 Confirmed as stated

Re-verified on this branch, unchanged: §1.1 (artifact branches share tree `dabdd4ef`);
§1.3 (both `mmu_midgard` files are 100% commented — 0 live lines in 63 + 558 — no
factory registers `"midgard"`, and six files carry the stale include); the delta is
exactly the two commits `eb59b82` and `073a463`.

---

## 3. The Virtuoso regression

A full pass is ~75 minutes and mostly irrelevant to this work. What is needed is a
**fast invariance check** plus a growing feature suite.

### 3.1 `testing/test_virtuoso.py` (build first, step **O0**)

Two modes:

- **`--invariance`** — the workhorse. Runs the existing Sniper configs
  (`baseline`, `parametric_baseline`, `userspace_baseline`, one `multicore`) and diffs
  `sim.stats` against a recorded golden set. A T0/T1 step **must** produce byte-identical
  output; the check fails loudly if it does not. This is the whole safety argument for
  the orthogonal-first ordering, so it runs after every step.
- **`--feature <name>`** — per-feature configs, added by the step that makes them
  runnable. Skips with a reason when the feature is not built, matching the existing
  suites' convention.

Golden stats are regenerated only by an explicit `--bless`, and a T2 step's commit must
say which numbers moved and why.

### 3.2 Cost control

Feature runs use the short traces in `testing/traces`. Target: the invariance check
under two minutes, the whole Virtuoso suite under ten. It is wired into `make regress`
as `regress_virtuoso` once it exists, and can be run alone during development.

---

## 4. Staged plan

Steps are named `O*` (orthogonal), `F*` (feature, config-gated), `I*` (invasive) rather
than reusing the source plan's letters, because the order differs. The source plan's
step ID is given for cross-reference.

### Stage O — inert additions (T0)

Nothing here can change a number. Each is build-only plus the invariance check.

| Step | Content | Source |
|---|---|---|
| **O0** | `testing/test_virtuoso.py`, golden stats for the four smoke configs | new |
| **O1** | `PDDM/misc/{bloom_filter.h, city.h, city.cc}` | §2.3 (not in source plan) |
| **O2** | `include/debug_config.h` additions, incl. the new `DEBUG_*` macros §2.3 of the source plan requires | A12 |
| **O3** | `MM/page_tables/payload_word.h` — the **file only**, no `pagetable.h` change | part of B1 |
| **O4** | `TP/ConfidencePolicy.h`, `TP/pte_offset_codec.{h,cc}` — no consumers yet | B3 |
| **O5** | DRAM detailed queueing stats; radix `prefetch_page_faults` split | A5, A11 |
| **O6** | `MMUCacheInterface` default no-op virtuals (interface header only) | part of A8 |

O5 registers new counters, so "byte-identical" means *modulo added counters* — the
invariance check compares the intersection and lists additions separately.

### Stage F — config-gated features (T1)

Each lands a subsystem reachable only by naming it. Invariance check must stay clean.

| Step | Content | Source | Notes |
|---|---|---|---|
| **F1** | Speculative-translation framework: `spec_engine_{base,factory}.h`, `mmu_spec.{h,cc}`, `"spec"` in `mmu_factory.h` | C1 | drop the stale `utopia`/`midgard`/`spot` includes |
| **F2** | Revelator allocators + policies | C3 | touches `physical_memory_allocator.h`, `buddy.h`, `buddy_policy.h` — read them as T2 and land that hunk separately if it is not purely additive |
| **F3** | Revelator engines (+ `numa_revelator`) | C4, C6 | needs O1; drop five stale includes (§2.5); **C2 dropped** (§2.4) |
| **F4** | Revelator exception handler | C5 | |
| **F5** | Victima MMU | C8 | `VICTIMA_TLB_BLOCK` is an additive enum value |
| **F6** | `hw_fault` MMU | G1 | cheap, self-contained, includes `eb59b82` |
| **F7** | Linux daemon models (kswapd/khugepaged/kcompactd) | D1 | default off; the `mimicos.{h,cc}` hooks are the only T2 risk — verify off ⇒ identical |
| **F8** | `linux_buddy_anon` header | D2 | |
| **F9** | Userspace MimicOS substrate: kernel arena, fault pipeline, scheduler | E7a/b/c | header-only; nothing calls it until Stage I |

After F5 the branch can run Revelator and Victima end-to-end — the two headline
features that need no memory-hierarchy surgery.

### Stage I — invasive, one number-moving change per commit (T2)

Ordered so each is individually attributable.

| Step | Content | Source | Expected effect |
|---|---|---|---|
| **I1** | Non-power-of-2 set index in `CacheBase` | A1 | no-op for power-of-2 caches |
| **I2** | TLB/PWC `HASH_MASK → HASH_MOD` | A2 | **changes baseline** for non-pow2 TLB/PWC |
| **I3** | `CacheBlockInfo` sub-line credit + fill-source | A7 | additive fields on a hot struct |
| **I4** | `CacheCntlr` implementations and stats | A8 | |
| **I5** | `mmu_base` perfect-L2-TLB / perfect-prefetch / prefetch accounting | A9 | moves prefetch tagging out of an early return |
| **I6** | `mmu.cc` | A10 | **fixes** an uncharged L2-TLB hit latency when page-size prediction is off |
| **I7** | `pagetable.h` shadow-payload type change to `PayloadWord` | rest of B1 | |
| **I8** | `TLBPrefetcherBase` signature + `tlb.cc` prefetch queue | B2 | touches every existing prefetcher |
| **I9** | `tlb_prefetcher_factory` restructure | B4 | |
| **I10** | **TRAIL** (`TemporalPTEPrefetcher`) + Berti | B5, B6 | the payoff for I1–I9 |
| **I11** | Optional: `atp`/`recency`/`dp`, data prefetchers | B7, B9 | only for paper-exact comparison |

### Stage K — frontend surgery (T2, highest risk, last)

The trace frontend and magic interface. This is where a mistake breaks every
configuration, not just a new one, so it lands after everything else is known good.

| Step | Content | Source |
|---|---|---|
| **K1** | Reserve the eight new magic commands as no-op stubs | E1 |
| **K2** | Per-thread ROI barrier | E2 |
| **K3** | SIFT recorder follows fork+execve children | E3 (re-derive, §2.1) |
| **K4** | Per-core MimicOS event queues | E4 |
| **K5** | Multi-core kernel pthreads, `AppReaderPool`, live-app scheduling | E5 |
| **K6** | Fast-fault replay path | E6 |
| **K7** | Userspace MimicOS v2 kernel integration | E7d |

K5 is the single largest item in the whole migration (`trace_manager` +
`trace_thread`, ~1,000 changed lines against files this branch has already modified).
It should be split further once K1–K4 are in and its real shape is visible.

### Not ported

As the source plan concludes, plus §2.4: Midgard (dead), `address_space.{h,cc}`
(unused), `fast_detailed` configs (deliberately removed from Sniper), the duplicate
`.cc` allocators (would auto-compile and collide, §2.2), and now **C2**.

---

## 5. Per-step discipline

1. One step, one commit, always building.
2. `testing/test_virtuoso.py` after every step. T0/T1: existing counters
   unchanged within tolerance, with any new counters listed separately.
3. **A feature step is not done until it has been run and shown to activate.**
   Building it and leaving the baselines alone proves only that it is harmless.
   Every `F*` step therefore lands three things together: the code, the
   configuration that selects it, and a `--feature` check asserting on counters
   the feature is specified to move. Without that, a feature that silently does
   nothing is indistinguishable from one that works -- Revelator disables itself
   in its constructor when the allocator does not suit it, and then runs to
   completion with a plausible IPC and every counter registered reading zero.
4. A T2 commit message states which statistics move and why.
5. Copy files verbatim from `09c00ea`, then apply the source plan's 2.2 (Sniper
   formatting wins) and 2.3 (debug-output policy) as a separate, reviewable pass.
6. Re-derive rather than apply any hunk against the five files in 2.1.

### 5.1 What running the features has already turned up

Findings that appeared only once a feature was actually executed. They are
recorded because they say what the remaining steps should expect.

- **The two trees initialise in different orders.** Virtuoso constructs MimicOS
  before CoreManager; Sniper did the opposite. CoreManager builds the cores, and
  with them the MMUs and any speculative engine, which query MimicOS as they are
  constructed. Revelator compiled, left every configuration byte-identical, and
  then segfaulted during startup. Fixed by moving the construction (`d671d1a`).
  Expect more of this: ported code that reaches for a global during construction
  is assuming an order this tree does not necessarily share.
- **F2 and F3 cannot be separated.** `revelator_thp_policy.h` dynamic_casts its
  spec engine to `RevelatorTHP*` and calls a method on it, so the allocator side
  does not compile without the engine side. They landed as one step.
- **Three shared-header edits the source plan lists for C3 are not needed**, and
  the compiler rather than a reading says so: `last_alloc_fastpath` is read only
  by `linux_buddy_anon.h`, and the "buddy" in the Revelator files is an enum
  value name, `ReservedBuddy`. `physical_memory_allocator.h`, `buddy.h` and
  `buddy_policy.h` are untouched, which is what keeps Revelator a T1 step.
- **Each Revelator engine header carried five stale includes, not two**, as 2.5
  predicted.
