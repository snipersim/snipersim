#include "zfstream.h"
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#include <algorithm>
#include <cassert>
#include <ios>

#if !SIFT_USE_ZLIB

ozstream::ozstream(vostream *output)
   : output(output)
{
   assert(false);
}

ozstream::~ozstream()
{
}

void ozstream::write(const char* s, std::streamsize n)
{
}

void ozstream::doCompress(bool finish)
{
}

izstream::izstream(vistream *input)
   : input(input)
   , m_eof(false)
   , m_fail(false)
   , peek_valid(false)
{
}

izstream::~izstream()
{
}

void izstream::read(char* s, std::streamsize n)
{
}

int izstream::peek()
{
   return 0;
}

#else /*SIFT_USE_ZLIB*/

#include <zlib.h>

ozstream::ozstream(vostream *output)
   : output(output)
{
   zstream.zalloc = Z_NULL;
   zstream.zfree = Z_NULL;
   zstream.opaque = Z_NULL;
   int ret = deflateInit(&zstream, level);
   assert(ret == Z_OK);
}

ozstream::~ozstream()
{
   doCompress(true);
   deflateEnd(&zstream);
   delete output;
}

void ozstream::write(const char* s, std::streamsize n)
{
   zstream.next_in = (Bytef*)s;
   zstream.avail_in = n;
   doCompress(false);
}

void ozstream::doCompress(bool finish)
{
   /* Consume all data in zstream.next_in and write it to the output stream */

   int ret;
   do
   {
      zstream.next_out = (Bytef*)buffer;
      zstream.avail_out = chunksize;
      ret = deflate(&zstream, finish ? Z_FINISH : Z_NO_FLUSH);
      assert(ret != Z_STREAM_ERROR);
      output->write(buffer, chunksize - zstream.avail_out);
   } while(zstream.avail_out == 0);
   assert(zstream.avail_in == 0);     /* all input will be used */
   if (finish)
      assert(ret == Z_STREAM_END);
}



izstream::izstream(vistream *input)
   : input(input)
   , m_eof(false)
   , m_fail(false)
   , peek_valid(false)
{
   zstream.zalloc = Z_NULL;
   zstream.zfree = Z_NULL;
   zstream.opaque = Z_NULL;
   zstream.avail_in = 0;
   zstream.next_in = Z_NULL;
   int ret = inflateInit(&zstream);
   assert(ret == Z_OK);
}

izstream::~izstream()
{
   inflateEnd(&zstream);
   delete input;
}

void izstream::read(char* s, std::streamsize n)
{
   if (peek_valid)
   {
      s[0] = peek_value;
      peek_valid = false;
      ++s;
      --n;
   }
   if (n == 0)
      return;

   zstream.next_out = (Bytef*)s;
   zstream.avail_out = n;

   do
   {
      if (zstream.avail_in == 0) // If input data was left over from the previous call, use that up first
      {
         input->read(buffer, chunksize);
         zstream.next_in = (Bytef*)buffer;
         zstream.avail_in = chunksize;
      }
      int ret = inflate(&zstream, Z_NO_FLUSH);
      if (ret == Z_STREAM_END) {
         m_eof = true;
         if (zstream.avail_out)
         {
            m_fail = true;
            return;
         }
      } else
         assert(ret == Z_OK);
   } while(zstream.avail_out != 0);
}

int izstream::peek()
{
   if (peek_valid == true)
      return peek_value;

   read(&peek_value, 1);
   peek_valid = true;

   return peek_value;
}

#endif /*SIFT_USE_ZLIB*/

#include <cstdio>
/* FIFO opens that do not wait inside the C library -- see cvofstream in
   zfstream.h for why.  A regular file is opened the same way and just works. */
static int open_reader_without_waiting(const char * filename)
{
	// O_RDONLY|O_NONBLOCK succeeds at once on a FIFO, writer or not; the
	// writer is awaited in cvifstream::fill().
	return ::open(filename, O_RDONLY | O_NONBLOCK);
}

static int open_writer_without_waiting(const char * filename)
{
	for (;;)
	{
		// On a FIFO with no reader this fails with ENXIO rather than waiting
		int fd = ::open(filename, O_WRONLY | O_NONBLOCK | O_CREAT | O_TRUNC, 0666);
		if (fd >= 0)
		{
			// The reader is there, so a blocking open returns at once; use
			// that fd, since fcntl(F_SETFL) cannot be relied on (PinCRT).
			int bfd = ::open(filename, O_WRONLY);
			::close(fd);
			return bfd;
		}
		if (errno != ENXIO)
			return fd;
		struct timespec ts = { 0, 1000000 };   // 1 ms: the reader is not there yet
		::nanosleep(&ts, NULL);
	}
}

cvofstream::cvofstream(const char * filename)
{
	int fd = open_writer_without_waiting(filename);
	this->stream = fd < 0 ? NULL : ::fdopen(fd, "wb");
}
cvofstream::~cvofstream()
{
	if (this->stream)
		std::fclose(this->stream);
}
void cvofstream::write(const char* s, std::streamsize n)
{
	std::fwrite(s, 1, n, this->stream);
}
void cvofstream::flush()
{
	std::fflush(this->stream);
}
bool cvofstream::fail()
{
	return this->stream == NULL || std::ferror(this->stream);
}
bool cvofstream::is_open()
{
	return this->stream != NULL;
}

cvifstream::cvifstream(const char * filename, std::ios_base::openmode mode)
	: fd(-1), filename(strdup(filename)), owned(NULL), writer_seen(false), peek_buffer(0), buffer_in_use(false), read_failed(false)
{
	// FIXME: Reimplement this correctly
	if(mode & (std::ios_base::app | std::ios_base::ate | std::ios_base::trunc | std::ios_base::out))
		assert(false);
	this->fd = open_reader_without_waiting(filename);
	assert(this->fd >= 0);
}
cvifstream::cvifstream(std::FILE *stream)
	: fd(::fileno(stream)), filename(NULL), owned(stream), writer_seen(true), peek_buffer(0), buffer_in_use(false), read_failed(false)
{
}
cvifstream::~cvifstream()
{
	if (this->owned)
		std::fclose(this->owned);
	else if (this->fd >= 0)
		::close(this->fd);
	free(this->filename);
}
/* Read exactly n bytes; false (and read_failed) on end of file or error.
   Until a writer has been seen the fd is non-blocking: 0 means no writer
   yet -- wait a millisecond and try again -- and EAGAIN means one is there. */
bool cvifstream::fill(char* s, size_t n)
{
	while (n > 0)
	{
		ssize_t r = ::read(this->fd, s, n);
		if (r > 0)
		{
			if (!this->writer_seen)
				this->switchToBlocking();
			s += r; n -= r;
			continue;
		}
		if (!this->writer_seen)
		{
			if (r < 0 && errno == EAGAIN)
			{
				this->switchToBlocking();
				continue;
			}
			if (r == 0 || (r < 0 && errno == EINTR))
			{
				struct timespec ts = { 0, 1000000 };
				::nanosleep(&ts, NULL);
				continue;
			}
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r == 0)
			std::cerr << "[SIFT] response pipe: end of file -- the simulator closed it" << std::endl;
		else
			std::cerr << "[SIFT] response pipe: read failed: " << strerror(errno) << std::endl;
		this->read_failed = true;    // end of file, or an error
		return false;
	}
	return true;
}
/* The writer is connected: reopen the FIFO in blocking mode (immediate, the
   writer holds its end) and drop the non-blocking fd.  Both fds share the
   pipe, so nothing already written is lost. */
void cvifstream::switchToBlocking()
{
	this->writer_seen = true;
	if (!this->filename)
		return;
	int bfd = ::open(this->filename, O_RDONLY);
	if (bfd >= 0)
	{
		::close(this->fd);
		this->fd = bfd;
	}
}
void cvifstream::read(char* s, std::streamsize n)
{
	if (n <= 0)
		return;
	if (this->buffer_in_use)
	{
		*s++ = this->peek_buffer; --n;
		this->buffer_in_use = false;
	}
	if (n > 0)
		this->fill(s, n);
}
int cvifstream::peek()
{
	if (!this->buffer_in_use)
	{
		if (!this->fill(&this->peek_buffer, 1))
			return EOF;
		this->buffer_in_use = true;
	}
	return (unsigned char)this->peek_buffer;
}
bool cvifstream::fail() const
{
	return this->read_failed;
}
