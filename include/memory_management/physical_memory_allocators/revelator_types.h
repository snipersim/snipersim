#pragma once

// ── Shared constants for Revelator-family allocators ─────────────────────────
static constexpr int    PAGE_SIZE_4KB_BITS   = 12;
static constexpr int    PAGE_SIZE_2MB_BITS   = 21;
static constexpr int    PAGES_IN_2MB_REGION  = 512;  // 2MB / 4KB

// ── Region state enum ────────────────────────────────────────────────────────
enum class RegionState : int {
    Free               = 0,   // Completely unused
    ReservedRevelator  = 1,   // Allocated as a full 2MB page by Revelator hash
    ReservedTHP        = 2,   // Reserved for THP (sub-page tracking via bitset)
    ReservedRevelator4KB = 3, // Contains 4KB pages placed by Revelator hash
    ReservedBuddy      = 4,   // Contains 4KB pages placed by fallback scan
    PoisonedTHP        = 5    // Poisoned at 2MB granularity (fragmentation)
};
