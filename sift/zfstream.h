#ifndef __ZFSTREAM_H
#define __ZFSTREAM_H

#include <zlib.h>
#include <ostream>
#include <istream>
#include <fstream>

class vostream
{
   public:
      virtual ~vostream() {}
      virtual void write(const char* s, std::streamsize n) = 0;
      virtual void flush() = 0;
};

class vofstream : public vostream
{
   private:
      std::ofstream stream;
   public:
      vofstream(const char * filename, std::ios_base::openmode mode = std::ios_base::out)
         : stream(filename, mode) {}
      virtual ~vofstream() {}
      virtual void write(const char* s, std::streamsize n)
         { stream.write(s, n); }
      virtual void flush()
         { stream.flush(); }
      virtual void fail()
         { stream.fail(); }
};

class ozstream : public vostream
{
   private:
      vostream *output;
      z_stream zstream;
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

class izstream : public vistream
{
   private:
      vistream *input;
      bool m_eof;
      bool m_fail;
      z_stream zstream;
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
