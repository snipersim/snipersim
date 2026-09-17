#ifndef LINE_OUT_H
#define LINE_OUT_H

/* One line, one write.
 *
 * std::cout << a << b << std::endl is several writes to the stream, and two
 * threads doing it at once interleave inside the line.  LineOut() collects
 * the whole expression and hands it to stdio as a single line in its
 * destructor, so a line is either wholly there or not at all -- from any
 * thread, and from any of the processes whose output run-sniper merges.
 *
 *    LineOut() << "[TRACE:" << id << "] -- " << what;
 *
 * std::endl and std::flush are accepted and ignored: the line ends itself.
 * Stream manipulators such as std::hex apply to the line being built.
 * Pair it with a line-buffered stdout (setvbuf in main) so a line reaches
 * the pipe when it is complete, not when the buffer happens to fill. */

#include <sstream>
#include <ostream>
#include <cstdio>

class LineOut
{
public:
   LineOut() {}
   ~LineOut()
   {
      m_line << '\n';
      std::fputs(m_line.str().c_str(), stdout);
   }
   template <typename T>
   LineOut& operator<<(const T& v) { m_line << v; return *this; }
   // std::hex, std::dec, std::boolalpha, ...
   LineOut& operator<<(std::ios_base& (*m)(std::ios_base&)) { m_line << m; return *this; }
   // std::endl, std::flush: the line ends in the destructor
   LineOut& operator<<(std::ostream& (*)(std::ostream&)) { return *this; }
private:
   std::ostringstream m_line;
   LineOut(const LineOut&);
   LineOut& operator=(const LineOut&);
};

#endif // LINE_OUT_H
