#ifndef __ZFSTREAM_H
#define __ZFSTREAM_H

#include "sift_format.h"

#include <ostream>
#include <istream>
#include <fstream>

#if SIFT_USE_ZLIB
# include <zlib.h>
#endif

class vostream
{
   public:
      virtual ~vostream() {}
      virtual void write(const char* s, std::streamsize n) = 0;
      virtual void flush() = 0;
      virtual bool is_open() = 0;
      virtual bool fail() = 0;
};

class vofstream : public vostream
{
   private:
      std::ofstream *stream;
   public:
      vofstream(const char * filename, std::ios_base::openmode mode = std::ios_base::out)
         : stream(new std::ofstream(filename, mode)) {}
      vofstream(std::ofstream *stream)
         : stream(stream) {}
      virtual ~vofstream() { delete stream; }
      virtual void write(const char* s, std::streamsize n)
         { stream->write(s, n); }
      virtual void flush()
         { stream->flush(); }
      virtual bool fail()
         { return stream->fail(); }
      virtual bool is_open()
         { return stream->is_open(); }
};

class ozstream : public vostream
{
   private:
      vostream *output;
#if SIFT_USE_ZLIB
      z_stream zstream;
#endif
      static const size_t chunksize = 64*1024;
      static const int level = 9;
      char buffer[chunksize];
      void doCompress(bool finish);
   public:
      ozstream(vostream *output);
      virtual ~ozstream();
      virtual void write(const char* s, std::streamsize n);
      virtual void flush()
         { output->flush(); }
      virtual bool fail()
         { return output->fail(); }
      virtual bool is_open()
         { return output->is_open(); }
};



class vistream
{
   public:
      virtual ~vistream() {}
      virtual void read(char* s, std::streamsize n) = 0;
      virtual int peek() = 0;
      virtual bool fail() const = 0;
};

class vifstream : public vistream
{
   private:
      std::ifstream *stream;
   public:
      vifstream(const char * filename, std::ios_base::openmode mode = std::ios_base::in)
         : stream(new std::ifstream(filename, mode)) {}
      vifstream(std::ifstream *stream)
         : stream(stream) {}
      virtual ~vifstream() { delete stream; }
      virtual void read(char* s, std::streamsize n)
         { stream->read(s, n); }
      virtual int peek()
         { return stream->peek(); }
      virtual bool fail() const { return stream->fail(); }
};

/* The response stream, read from a FIFO whose writer (a Sniper trace thread)
   may connect late: with more threads than cores, that thread may be waiting
   for a core.  The open never waits (O_RDONLY|O_NONBLOCK succeeds at once),
   and the writer is awaited in the first read, holding no lock: a
   non-blocking read returns 0 while no writer is connected and EAGAIN once
   one is; then the FIFO is reopened in blocking mode -- immediate, the
   writer holds its end, and the pipe buffer is shared -- and the
   non-blocking fd closed.  (Not fcntl(F_SETFL): PinCRT's is a no-op.)  End
   of file afterwards means the writer went away, as before.  Raw fd, no
   stdio: PinCRT's fopen() blocks on a FIFO while holding its stdio lock,
   and every other tool thread's fopen then spins on it -- the
   oversubscription deadlock. */
class cvifstream : public vistream
{
   private:
	   int fd;
	   char *filename;            // for the blocking reopen once the writer is there
	   std::FILE *owned;          // when constructed from a FILE*, closed with it
	   bool writer_seen;
	   char peek_buffer;
	   bool buffer_in_use;
	   // Sticky: set when a read returns fewer bytes than asked for, whether
	   // from end of file or from a stream error, and reported by fail().
	   bool read_failed;
	   bool fill(char* s, size_t n);
	   void switchToBlocking();
   public:
	   cvifstream(const char * filename, std::ios_base::openmode mode = std::ios_base::in);
	   cvifstream(std::FILE *stream);
	   virtual ~cvifstream();
	   virtual void read(char* s, std::streamsize n);
	   virtual int peek();
	   virtual bool fail() const;
};

/* A FILE*-backed output stream whose FIFO open never blocks inside the C
   library.  The recorder runs inside Pin, where fopen() on a FIFO that has
   no reader yet blocks while holding PinCRT's stdio lock, and every other
   tool thread's fopen -- a new thread's start, for one -- then spins on it.
   With more threads than cores the reader can be a while: it belongs to a
   trace thread that may be waiting for a core.  So the open is done with a
   raw open() that fails instead of waiting, retried with a short sleep and
   no lock held, and only the fd is handed to stdio. */
class cvofstream : public vostream
{
   private:
      std::FILE *stream;
      char *filename;
      bool reported;      // a failed write has been reported (once per stream)
   public:
      cvofstream(const char * filename);
      virtual ~cvofstream();
      virtual void write(const char* s, std::streamsize n);
      virtual void flush();
      virtual bool fail();
      virtual bool is_open();
};

class izstream : public vistream
{
   private:
      vistream *input;
      bool m_eof;
      bool m_fail;
#if SIFT_USE_ZLIB
      z_stream zstream;
#endif
      static const size_t chunksize = 64*1024;
      char buffer[chunksize];
      char peek_value;
      bool peek_valid;
   public:
      izstream(vistream *input);
      virtual ~izstream();
      virtual void read(char* s, std::streamsize n);
      virtual int peek();
      virtual bool eof() const { return m_eof; }
      virtual bool fail() const { return m_fail; }
};

#endif // __ZFSTREAM_H
