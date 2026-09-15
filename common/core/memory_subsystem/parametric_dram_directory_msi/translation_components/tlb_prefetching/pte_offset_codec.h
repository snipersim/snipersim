#pragma once

#include <cstdint>
#include <vector>
#include <cassert>
#include <string>
#include <sstream>
#include "payload_word.h"

namespace ParametricDramDirectoryMSI
{

    /**
     * @brief A single decoded offset entry from a PTE payload.
     *
     * Each slot in the PTE payload stores a signed delta (ΔVPN) and an
     * optional confidence counter.
     */
    struct PTEOffsetEntry
    {
        int32_t  delta_vpn;   ///< Signed decoded offset (two's complement)
        uint32_t conf;        ///< Decoded confidence (0 when conf_bits == 0)
    };

    /**
     * @brief Configuration for PTE offset encoding/decoding.
     *
     * Defines the bit layout of k offset+confidence slots packed inside
     * a uint64_t PTE payload value.  Slot i occupies bits:
     *
     *   [base_bit + i*(offset_bits+conf_bits) .. base_bit + (i+1)*(offset_bits+conf_bits) - 1]
     *
     * The offset field comes first (lower bits), followed by the confidence
     * field (higher bits) within each slot.
     */
    struct PTEOffsetConfig
    {
        uint32_t num_entries;      ///< k — number of delta+conf slots
        uint32_t offset_bits;      ///< b_off — bits per signed offset
        uint32_t conf_bits;        ///< b_conf — bits per confidence (can be 0)
        uint32_t base_bit;         ///< start bit within payload
        bool     signed_offset;    ///< true → two's complement sign extension

        /** Total bits consumed by all slots. */
        uint32_t totalBits() const { return num_entries * (offset_bits + conf_bits); }

        /** Maximum representable confidence value. */
        uint32_t maxConf() const { return conf_bits > 0 ? ((1u << conf_bits) - 1) : 0; }

        /** Validate that the layout fits in the 256-bit payload word. */
        bool fitsInPayload() const { return (base_bit + totalBits()) <= PayloadWord::BITS; }
    };

    /**
     * @brief Encoder/decoder for per-page temporal offset entries packed
     *        into a PTE payload word.
     *
     * The codec is stateless: it only needs the configuration and a raw
     * uint64_t payload value.  "Where bits live" is fully abstract — the
     * caller provides the payload word.
     */
    class PTEOffsetCodec
    {
    public:
        explicit PTEOffsetCodec(PTEOffsetConfig cfg)
            : m_cfg(cfg)
        {
            assert(cfg.fitsInPayload() && "PTE offset slots exceed 256-bit payload");
            assert(cfg.offset_bits > 0 && "offset_bits must be > 0");
            assert(cfg.num_entries > 0 && "num_entries must be > 0");
        }

        const PTEOffsetConfig& config() const { return m_cfg; }

        // -----------------------------------------------------------------
        // Decode: extract all k entries from a raw payload
        // -----------------------------------------------------------------
        std::vector<PTEOffsetEntry> decode(const PayloadWord& pte_value) const
        {
            std::vector<PTEOffsetEntry> entries;
            entries.reserve(m_cfg.num_entries);

            for (uint32_t i = 0; i < m_cfg.num_entries; i++)
            {
                uint32_t slot_base = m_cfg.base_bit + i * slotWidth();

                // Extract offset field
                uint64_t raw_offset = PayloadWord::getField(pte_value, slot_base, m_cfg.offset_bits);
                int32_t delta = m_cfg.signed_offset
                                    ? signExtend(raw_offset, m_cfg.offset_bits)
                                    : static_cast<int32_t>(raw_offset);

                // Extract confidence field
                uint32_t conf = 0;
                if (m_cfg.conf_bits > 0)
                {
                    conf = static_cast<uint32_t>(
                        PayloadWord::getField(pte_value, slot_base + m_cfg.offset_bits, m_cfg.conf_bits));
                }

                entries.push_back({delta, conf});
            }
            return entries;
        }

        // -----------------------------------------------------------------
        // Encode / update: insert or bump a delta in the best slot
        //
        // Strategy:
        //   1. If `new_delta` already exists in some slot → bump its conf
        //   2. Else pick a victim slot (lowest confidence, tie-break = first)
        //      and overwrite with (new_delta, init_conf)
        //
        // Returns the new payload value.
        // -----------------------------------------------------------------
        PayloadWord encode_update(const PayloadWord& old_pte_value,
                               int32_t  new_delta,
                               uint32_t init_conf = 1) const
        {
            auto entries = decode(old_pte_value);

            // 1. Check if delta already present
            int match_idx = -1;
            for (uint32_t i = 0; i < entries.size(); i++)
            {
                if (entries[i].delta_vpn == new_delta)
                {
                    match_idx = static_cast<int>(i);
                    break;
                }
            }

            if (match_idx >= 0)
            {
                // Bump confidence (saturating)
                uint32_t max_c = m_cfg.maxConf();
                entries[match_idx].conf = (entries[match_idx].conf < max_c)
                                              ? entries[match_idx].conf + 1
                                              : max_c;
            }
            else
            {
                // Find victim: slot with lowest confidence (first wins ties)
                uint32_t victim = 0;
                uint32_t min_conf = entries[0].conf;
                for (uint32_t i = 1; i < entries.size(); i++)
                {
                    if (entries[i].conf < min_conf)
                    {
                        min_conf = entries[i].conf;
                        victim = i;
                    }
                }
                entries[victim].delta_vpn = new_delta;
                entries[victim].conf = init_conf;
            }

            // Re-encode
            return encodeAll(old_pte_value, entries);
        }

        // -----------------------------------------------------------------
        // Encode from scratch: write all entries into a (possibly zero)
        // payload, preserving bits outside the slot region.
        // -----------------------------------------------------------------
        PayloadWord encodeAll(const PayloadWord& base_value,
                           const std::vector<PTEOffsetEntry>& entries) const
        {
            assert(entries.size() == m_cfg.num_entries);
            PayloadWord result = base_value;

            // The slots tile the region [base_bit, base_bit+totalBits) contiguously,
            // so writing every offset+conf field fully overwrites the slot region;
            // bits outside the region are preserved from base_value.
            for (uint32_t i = 0; i < m_cfg.num_entries; i++)
            {
                uint32_t slot_base = m_cfg.base_bit + i * slotWidth();

                // Encode offset (setField masks to offset_bits)
                PayloadWord::setField(result, slot_base, m_cfg.offset_bits,
                                      static_cast<uint64_t>(entries[i].delta_vpn));

                // Encode confidence
                if (m_cfg.conf_bits > 0)
                    PayloadWord::setField(result, slot_base + m_cfg.offset_bits,
                                          m_cfg.conf_bits, entries[i].conf);
            }
            return result;
        }

        // -----------------------------------------------------------------
        // Confidence decay: decrement all slot confidences by 1 (floor 0)
        // -----------------------------------------------------------------
        PayloadWord decayAllConf(const PayloadWord& pte_value) const
        {
            if (m_cfg.conf_bits == 0) return pte_value;

            auto entries = decode(pte_value);
            for (auto& e : entries)
            {
                if (e.conf > 0) e.conf--;
            }
            return encodeAll(pte_value, entries);
        }

        // -----------------------------------------------------------------
        // Debug: human-readable dump
        // -----------------------------------------------------------------
        std::string dumpPayload(const PayloadWord& pte_value) const
        {
            auto entries = decode(pte_value);
            std::ostringstream ss;
            ss << "PTE_payload{";
            for (uint32_t i = 0; i < entries.size(); i++)
            {
                if (i > 0) ss << ", ";
                ss << "slot" << i << ":(Δ=" << entries[i].delta_vpn
                   << " conf=" << entries[i].conf << ")";
            }
            ss << "}";
            return ss.str();
        }

    private:
        PTEOffsetConfig m_cfg;

        uint32_t slotWidth() const { return m_cfg.offset_bits + m_cfg.conf_bits; }

        static int32_t signExtend(uint64_t val, uint32_t width)
        {
            // Two's complement sign extension from `width` bits to int32_t
            if (width == 0) return 0;
            uint64_t sign_bit = 1ULL << (width - 1);
            // If sign bit set, extend with 1s
            if (val & sign_bit)
            {
                // Fill upper bits with 1s
                uint64_t mask = ~((1ULL << width) - 1);
                val |= mask;
            }
            return static_cast<int32_t>(val);
        }
    };

} // namespace ParametricDramDirectoryMSI
