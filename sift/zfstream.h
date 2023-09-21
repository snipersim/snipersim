#ifndef __ZFSTREAM_H
#define __ZFSTREAM_H

#include "sift_format.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

#include <iostream>

#if SIFT_USE_ZLIB
# include <zlib.h>
#endif

class vostream
{
   public:
      virtual ~vostream() {}
      virtual void write(const char* s, size_t n) = 0;
      virtual void flush() = 0;
      virtual bool is_open() = 0;
      virtual bool fail() = 0;
};

class vofstream : public vostream
{
   private:
      int stream = -1;
   public:
      vofstream(const char * filename, int flags = O_WRONLY | O_CREAT | O_TRUNC, int mode = S_IRWXU)
         {
            stream = ::open(filename, flags, mode);
            if (stream < 0) {
               perror("[SIFT-vofstream]");
               fprintf(stderr, "Unable to open file [%s]\n", filename);
            }
         }
      vofstream(int _stream)
         : stream(_stream) {}
      virtual ~vofstream() {}
      virtual void write(const char* s, size_t n)
         {
            if (stream < 0) {
               return;
            }
            while (n > 0) {
               ssize_t w = ::write(stream, s, n);
               if (w < 0) {
		  perror("write");
                  close(stream);
                  stream = -1;
		  return;
               } else { // w <= n
                  n -= w;
                  s += w;
               }
            }
         }
      virtual void flush()
         {}
      virtual bool fail()
         { return (stream < 0); }
      virtual bool is_open()
         { return (stream >= 0); }
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
      virtual void write(const char* s, size_t n);
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
      virtual void read(char* s, size_t n) = 0;
      virtual int peek() = 0;
      virtual bool fail() const = 0;
      virtual size_t tellg() const = 0;
      virtual bool is_open() = 0;
};

class vifstream : public vistream
{
   private:
      int stream = -1;
      size_t count = 0;
      char peek_value = 0;
      bool peek_valid = false;
   public:
      vifstream(const char * filename, int flags = O_RDONLY)
         : stream(open(filename, flags)) {}
      vifstream(int _stream)
         : stream(_stream) {}
      virtual ~vifstream() {}
      virtual void read(char* s, size_t n)
         {
            if (stream < 0) {
              return;
            }
            if (n == 0) {
               return;
            }
            if (peek_valid) // n > 0
            {
               s[0] = peek_value;
               peek_valid = false;
               ++s;
               --n;
              ++count;
            }
            while (n > 0) {
               ssize_t r = ::read(stream, s, n);
               if (r < 0) {
                  close(stream);
                 stream = -1;
               } else { // r <= n
                  n -= r;
                  s += r;
                 count += r;
               }
            }
         }
      virtual int peek()
      {
         if (peek_valid == true) {
            return peek_value;
         }
         this->read(&peek_value, 1);
         peek_valid = true;

         return peek_value;
      }
      virtual bool fail() const { return (stream < 0); }
      virtual size_t tellg() const { return count; }
      virtual bool is_open() { return (stream >= 0); }
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
      virtual void read(char* s, size_t n);
      virtual int peek();
      virtual bool eof() const { return m_eof; }
      virtual bool fail() const { return m_fail; }
      virtual size_t tellg() const { return 0; }
      virtual bool is_open() { return input->is_open(); }
};

#endif // __ZFSTREAM_H
