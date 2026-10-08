#ifndef FOLDED_HISTORY_H
#define FOLDED_HISTORY_H

#include <cstdint>
#include <cstddef>
#include <vector>

/* The last `length` global-history bits folded (XORed in `width`-bit chunks)
   into one `width`-bit value, kept up to date one branch at a time: the new
   bit enters at the bottom, the bit leaving the window is cancelled at the
   position it was folded to (length mod width), and whatever is shifted past
   the top is folded back in.  This is the circular-shift-register form of
   Michaud's folded history used by TAGE and the hashed perceptron; the value
   equals the XOR of the window's width-bit chunks, newest bit lowest. */
class FoldedHistory
{
public:
   FoldedHistory(size_t length, size_t width)
      : m_length(length), m_width(width), m_mask((width >= 64) ? ~0ULL : ((1ULL << width) - 1))
      , m_value(0), m_history(length, false), m_head(0) {}

   void push(bool taken)
   {
      if (m_length == 0)
         return;
      bool leaving = m_history[m_head];
      m_history[m_head] = taken;
      m_head = (m_head + 1) % m_length;
      m_value = (m_value << 1) | (taken ? 1 : 0);
      m_value ^= (leaving ? 1ULL : 0ULL) << (m_length % m_width);
      m_value ^= m_value >> m_width;
      m_value &= m_mask;
   }

   uint64_t value() const { return m_value; }

private:
   size_t m_length, m_width;
   uint64_t m_mask, m_value;
   std::vector<bool> m_history;
   size_t m_head;
};

#endif /* FOLDED_HISTORY_H */
