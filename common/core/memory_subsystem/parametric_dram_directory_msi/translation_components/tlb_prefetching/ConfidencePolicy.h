#pragma once

// ============================================================================
// ConfidencePolicy — pluggable confidence update / decay strategies for the
// TemporalPTEPrefetcher.
//
// Each policy defines how confidence counters are manipulated when a matching
// delta is found (bump) or not found (miss) during learnTransition().
//
// Usage:
//   auto policy = ConfidencePolicy::create("competitive", bump, decay_bump, decay_miss);
//   auto result = policy->apply(entries, start, end, found, found_slot, max_conf);
//   // caller uses result to update stats
// ============================================================================

#include "pte_offset_codec.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ParametricDramDirectoryMSI
{

// Return value from ConfidencePolicy::apply()
struct ConfPolicyResult
{
    bool bumped;        ///< A matching slot was bumped
    bool any_decayed;   ///< At least one slot's confidence was decremented
};

// Abstract interface
class ConfidencePolicy
{
public:
    virtual ~ConfidencePolicy() = default;

    /// Apply bump (if found) and decay logic to the decoded entries.
    /// @param entries    Mutable decoded PTE slots
    /// @param slot_start First slot in the allowed range
    /// @param slot_end   One past the last slot in the allowed range
    /// @param found      True if a matching delta was found
    /// @param found_slot Index of the matching slot (only valid when found==true)
    /// @param max_conf   Maximum representable confidence (from codec)
    virtual ConfPolicyResult apply(std::vector<PTEOffsetEntry>& entries,
                                   uint32_t slot_start, uint32_t slot_end,
                                   bool found, uint32_t found_slot,
                                   uint32_t max_conf) = 0;

    virtual const char* name() const = 0;

    // Factory
    static std::unique_ptr<ConfidencePolicy> create(const std::string& policy_name,
                                                     uint32_t bump_amount,
                                                     uint32_t decay_on_bump,
                                                     uint32_t decay_on_miss);
};

// ============================================================================
// Policy: "none" — bump only, no competitive decay
// ============================================================================

class ConfidencePolicyNone : public ConfidencePolicy
{
    uint32_t m_bump_amount;
public:
    explicit ConfidencePolicyNone(uint32_t bump_amount)
        : m_bump_amount(bump_amount) {}

    ConfPolicyResult apply(std::vector<PTEOffsetEntry>& entries,
                           uint32_t slot_start, uint32_t slot_end,
                           bool found, uint32_t found_slot,
                           uint32_t max_conf) override
    {
        if (found)
        {
            uint32_t c = entries[found_slot].conf + m_bump_amount;
            entries[found_slot].conf = (c > max_conf) ? max_conf : c;
        }
        return {found, false};
    }

    const char* name() const override { return "none"; }
};

// ============================================================================
// Policy: "competitive" — linear decay of configurable amounts
//
//   On bump:  increment found_slot by bump_amount (saturating at max_conf),
//             decrement all OTHER slots in range by decay_on_bump (floor 0).
//   On miss:  decrement ALL slots in range by decay_on_miss (floor 0).
//
//   With bump=1, decay_on_bump=1, decay_on_miss=1 this matches the original
//   hardcoded behavior.
// ============================================================================

class ConfidencePolicyCompetitive : public ConfidencePolicy
{
    uint32_t m_bump_amount;
    uint32_t m_decay_on_bump;
    uint32_t m_decay_on_miss;
public:
    ConfidencePolicyCompetitive(uint32_t bump_amount,
                                uint32_t decay_on_bump,
                                uint32_t decay_on_miss)
        : m_bump_amount(bump_amount),
          m_decay_on_bump(decay_on_bump),
          m_decay_on_miss(decay_on_miss) {}

    ConfPolicyResult apply(std::vector<PTEOffsetEntry>& entries,
                           uint32_t slot_start, uint32_t slot_end,
                           bool found, uint32_t found_slot,
                           uint32_t max_conf) override
    {
        bool any_decayed = false;

        if (found)
        {
            // Bump the matched slot
            uint32_t c = entries[found_slot].conf + m_bump_amount;
            entries[found_slot].conf = (c > max_conf) ? max_conf : c;

            // Decay all OTHER slots
            if (m_decay_on_bump > 0)
            {
                for (uint32_t i = slot_start; i < slot_end; i++)
                {
                    if (i == found_slot) continue;
                    if (entries[i].conf > 0)
                    {
                        entries[i].conf = (entries[i].conf > m_decay_on_bump)
                                          ? entries[i].conf - m_decay_on_bump : 0;
                        any_decayed = true;
                    }
                }
            }
        }
        else
        {
            // Miss: decay ALL slots
            if (m_decay_on_miss > 0)
            {
                for (uint32_t i = slot_start; i < slot_end; i++)
                {
                    if (entries[i].conf > 0)
                    {
                        entries[i].conf = (entries[i].conf > m_decay_on_miss)
                                          ? entries[i].conf - m_decay_on_miss : 0;
                        any_decayed = true;
                    }
                }
            }
        }

        return {found, any_decayed};
    }

    const char* name() const override { return "competitive"; }
};

// ============================================================================
// Policy: "exponential" — decay by halving (right-shift by 1)
//
//   On bump:  increment found_slot by bump_amount, halve all OTHER slots.
//   On miss:  halve ALL slots.
//   Faster decay than linear, good for rapidly adapting to phase changes.
// ============================================================================

class ConfidencePolicyExponential : public ConfidencePolicy
{
    uint32_t m_bump_amount;
public:
    explicit ConfidencePolicyExponential(uint32_t bump_amount)
        : m_bump_amount(bump_amount) {}

    ConfPolicyResult apply(std::vector<PTEOffsetEntry>& entries,
                           uint32_t slot_start, uint32_t slot_end,
                           bool found, uint32_t found_slot,
                           uint32_t max_conf) override
    {
        bool any_decayed = false;

        if (found)
        {
            uint32_t c = entries[found_slot].conf + m_bump_amount;
            entries[found_slot].conf = (c > max_conf) ? max_conf : c;

            for (uint32_t i = slot_start; i < slot_end; i++)
            {
                if (i == found_slot) continue;
                if (entries[i].conf > 0)
                {
                    entries[i].conf >>= 1;
                    any_decayed = true;
                }
            }
        }
        else
        {
            for (uint32_t i = slot_start; i < slot_end; i++)
            {
                if (entries[i].conf > 0)
                {
                    entries[i].conf >>= 1;
                    any_decayed = true;
                }
            }
        }

        return {found, any_decayed};
    }

    const char* name() const override { return "exponential"; }
};

// ============================================================================
// Policy: "bump_only_strong" — bump by 2, no decay
//
//   Stronger reinforcement for confirmed deltas, but no pressure on stale
//   entries.  Useful for stable workloads with few phase changes.
// ============================================================================

class ConfidencePolicyBumpOnlyStrong : public ConfidencePolicy
{
public:
    ConfPolicyResult apply(std::vector<PTEOffsetEntry>& entries,
                           uint32_t slot_start, uint32_t slot_end,
                           bool found, uint32_t found_slot,
                           uint32_t max_conf) override
    {
        if (found)
        {
            uint32_t c = entries[found_slot].conf + 2;
            entries[found_slot].conf = (c > max_conf) ? max_conf : c;
        }
        return {found, false};
    }

    const char* name() const override { return "bump_only_strong"; }
};

// ============================================================================
// Factory
// ============================================================================

inline std::unique_ptr<ConfidencePolicy>
ConfidencePolicy::create(const std::string& policy_name,
                          uint32_t bump_amount,
                          uint32_t decay_on_bump,
                          uint32_t decay_on_miss)
{
    if (policy_name == "none")
        return std::make_unique<ConfidencePolicyNone>(bump_amount);

    if (policy_name == "competitive")
        return std::make_unique<ConfidencePolicyCompetitive>(bump_amount, decay_on_bump, decay_on_miss);

    if (policy_name == "exponential")
        return std::make_unique<ConfidencePolicyExponential>(bump_amount);

    if (policy_name == "bump_only_strong")
        return std::make_unique<ConfidencePolicyBumpOnlyStrong>();

    // Default to competitive
    return std::make_unique<ConfidencePolicyCompetitive>(bump_amount, decay_on_bump, decay_on_miss);
}

} // namespace ParametricDramDirectoryMSI
