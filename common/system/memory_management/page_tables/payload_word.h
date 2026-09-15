#pragma once
#include <cstdint>

namespace ParametricDramDirectoryMSI
{
    // ----------------------------------------------------------------------
    // PayloadWord — a 256-bit unsigned word used to hold the temporal-PTE
    // prefetcher payload (delta+confidence slots).
    //
    // GCC 11 has no native 256-bit integer (__int128 is the widest, _BitInt
    // needs GCC 14+), so we store the word as 4 little-endian 64-bit limbs
    // (w[0] = bits 0..63).  The codec only ever extracts/inserts fields up to
    // 64 bits wide that fit within the 256-bit word, so no full 256-bit
    // arithmetic is required — just the bit-field get/set helpers below, each
    // of which touches at most two adjacent limbs (a field never straddles
    // more than a 2-limb / 128-bit window).
    // ----------------------------------------------------------------------
    struct PayloadWord
    {
        static constexpr unsigned BITS  = 256;
        static constexpr unsigned LIMBS = 4;
        uint64_t w[LIMBS];

        PayloadWord() : w{0, 0, 0, 0} {}
        PayloadWord(uint64_t v) : w{v, 0, 0, 0} {}   // non-explicit: allows `= 0`

        uint64_t toUint64() const { return w[0]; }   // low 64 bits (lossy)
        bool isZero() const { return !(w[0] | w[1] | w[2] | w[3]); }

        friend bool operator==(const PayloadWord& a, uint64_t v)
        { return a.w[0] == v && !(a.w[1] | a.w[2] | a.w[3]); }
        friend bool operator!=(const PayloadWord& a, uint64_t v) { return !(a == v); }
        friend bool operator==(const PayloadWord& a, const PayloadWord& b)
        { return a.w[0] == b.w[0] && a.w[1] == b.w[1] && a.w[2] == b.w[2] && a.w[3] == b.w[3]; }
        friend bool operator!=(const PayloadWord& a, const PayloadWord& b) { return !(a == b); }

        // Extract `width` (<=64) bits starting at bit `start`.  Caller must
        // ensure start + width <= 256.
        static uint64_t getField(const PayloadWord& p, unsigned start, unsigned width)
        {
            if (width == 0) return 0;
            const unsigned wi = start >> 6;          // limb index
            const unsigned bo = start & 63;          // bit offset within limb
            __uint128_t window = (__uint128_t)p.w[wi]
                               | ((wi + 1 < LIMBS) ? ((__uint128_t)p.w[wi + 1] << 64) : 0);
            const __uint128_t mask = (width >= 64) ? ~(__uint128_t)0
                                                   : (((__uint128_t)1 << width) - 1);
            return (uint64_t)((window >> bo) & mask);
        }

        // Insert `value` into `width` (<=64) bits starting at bit `start`.
        static void setField(PayloadWord& p, unsigned start, unsigned width, uint64_t value)
        {
            if (width == 0) return;
            const uint64_t vmask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
            value &= vmask;
            const unsigned wi = start >> 6;
            const unsigned bo = start & 63;
            const __uint128_t fmask = (__uint128_t)vmask << bo;
            const __uint128_t ins   = (__uint128_t)value << bo;
            __uint128_t window = (__uint128_t)p.w[wi]
                               | ((wi + 1 < LIMBS) ? ((__uint128_t)p.w[wi + 1] << 64) : 0);
            window = (window & ~fmask) | ins;
            p.w[wi] = (uint64_t)window;
            if (wi + 1 < LIMBS) p.w[wi + 1] = (uint64_t)(window >> 64);
        }
    };

} // namespace ParametricDramDirectoryMSI
