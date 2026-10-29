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
#if !defined(PIN_CRT)
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <cassert>
#include <ios>
#if !defined(PIN_CRT)
# include <signal.h>
# include <pthread.h>
#endif

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

/* The open(2) behind the writer.  A DynamoRIO client's libc open() is
   redirected by DR's private loader to its own os_open_protected(), which
   takes DR's flag set rather than POSIX's and so fails on these flags (with
   errno untouched); the direct syscall is not redirected.  PinCRT has no
   syscall(), and Pin does not redirect open(), so it keeps the libc call. */
static int raw_open(const char * filename, int flags, int mode)
{
#if defined(PIN_CRT)
	return ::open(filename, flags, mode);
#else
	return (int) ::syscall(SYS_openat, AT_FDCWD, filename, flags, mode);
#endif
}

static int open_writer_without_waiting(const char * filename)
{
	for (;;)
	{
		// On a FIFO with no reader this fails with ENXIO rather than waiting
		int fd = raw_open(filename, O_WRONLY | O_NONBLOCK | O_CREAT | O_TRUNC, 0666);
		if (fd >= 0)
		{
			// The reader is there, so a blocking open returns at once; use
			// that fd, since fcntl(F_SETFL) cannot be relied on (PinCRT).
			int bfd = raw_open(filename, O_WRONLY, 0);
			::close(fd);
			if (bfd < 0)
				std::cerr << "[SIFT] open(" << filename << ", O_WRONLY) failed: " << strerror(errno) << std::endl;
			return bfd;
		}
		if (errno != ENXIO)
		{
			std::cerr << "[SIFT] open(" << filename << ", O_WRONLY|O_NONBLOCK|O_CREAT) failed: " << strerror(errno) << std::endl;
			return fd;
		}
		struct timespec ts = { 0, 1000000 };   // 1 ms: the reader is not there yet
		::nanosleep(&ts, NULL);
	}
}

bool cvofstream::s_guard_sigpipe = false;

/* stdio's buffer when SIGPIPE is guarded: its size is ours to know, so that
   only a write that can reach the pipe pays for the guard */
static const size_t GUARDED_BUFFER = 64 * 1024;

/* SIGPIPE held back in this thread for one write that may reach the pipe,
   and, if that write raised it, taken back before the thread's mask is
   restored (a signal pending before, from elsewhere, is left alone) */
class SigpipeGuard
{
#if !defined(PIN_CRT)
	sigset_t pipe, old;
	bool pending_before;
   public:
	SigpipeGuard()
	{
		sigemptyset(&this->pipe);
		sigaddset(&this->pipe, SIGPIPE);
		pthread_sigmask(SIG_BLOCK, &this->pipe, &this->old);
		sigset_t pending;
		this->pending_before = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE);
	}
	~SigpipeGuard()
	{
		int saved = errno;
		if (!this->pending_before)
		{
			struct timespec zero = { 0, 0 };
			while (sigtimedwait(&this->pipe, NULL, &zero) == SIGPIPE)
				;
		}
		pthread_sigmask(SIG_SETMASK, &this->old, NULL);
		errno = saved;
	}
#else
   public:
	SigpipeGuard() {}         // never asked for: the recorder ignores SIGPIPE
#endif
};

cvofstream::cvofstream(const char * filename)
	: stream(NULL), filename(strdup(filename)), reported(false), buffered(0)
{
	int fd = open_writer_without_waiting(filename);
	this->stream = fd < 0 ? NULL : ::fdopen(fd, "wb");
	if (fd >= 0 && !this->stream)
	{
		std::cerr << "[SIFT] fdopen(" << filename << ") failed: " << strerror(errno) << std::endl;
		::close(fd);
	}
	if (this->stream && s_guard_sigpipe)
		std::setvbuf(this->stream, NULL, _IOFBF, GUARDED_BUFFER);
}
cvofstream::~cvofstream()
{
	if (this->stream)
	{
		if (s_guard_sigpipe)
		{
			SigpipeGuard guard;
			std::fclose(this->stream);
		}
		else
			std::fclose(this->stream);
	}
	free(this->filename);
}
/* A write the simulator never receives ends the thread's stream short, and
   the simulator can only report the end, not why: say why here, once. */
void cvofstream::report()
{
	if (this->reported)
		return;
	this->reported = true;
	std::cerr << "[SIFT] write to " << this->filename << " failed: " << strerror(errno)
	          << " -- the simulator will see this thread's stream end early" << std::endl;
}
void cvofstream::write(const char* s, std::streamsize n)
{
	/* A write that fits in what is left of the buffer stays in it; one that
	   does not may reach the pipe, and is followed by a flush, so that the
	   buffer is known to be empty again */
	if (s_guard_sigpipe && this->buffered + n >= GUARDED_BUFFER)
	{
		SigpipeGuard guard;
		if (std::fwrite(s, 1, n, this->stream) != (size_t)n || std::fflush(this->stream) != 0)
			this->report();
		this->buffered = 0;
		return;
	}
	if (std::fwrite(s, 1, n, this->stream) != (size_t)n)
		this->report();
	this->buffered += n;
}
void cvofstream::flush()
{
	int r;
	if (s_guard_sigpipe)
	{
		SigpipeGuard guard;
		r = std::fflush(this->stream);
	}
	else
		r = std::fflush(this->stream);
	this->buffered = 0;
	if (r != 0)
		this->report();
}
bool cvofstream::fail()
{
	return this->stream == NULL || std::ferror(this->stream);
}
bool cvofstream::is_open()
{
	return this->stream != NULL;
}
int cvofstream::fd()
{
	return this->stream ? ::fileno(this->stream) : -1;
}

cvifstream::cvifstream(const char * filename, std::ios_base::openmode mode)
	: fd(-1), filename(strdup(filename)), owned(NULL), writer_seen(false), peek_buffer(0), buffer_in_use(false), read_failed(false), peer_fd(-1)
{
	// FIXME: Reimplement this correctly
	if(mode & (std::ios_base::app | std::ios_base::ate | std::ios_base::trunc | std::ios_base::out))
		assert(false);
	this->fd = open_reader_without_waiting(filename);
	assert(this->fd >= 0);
}
cvifstream::cvifstream(std::FILE *stream)
	: fd(::fileno(stream)), filename(NULL), owned(stream), writer_seen(true), peek_buffer(0), buffer_in_use(false), read_failed(false), peer_fd(-1)
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
				if (this->peerGone())
				{
					std::cerr << "[SIFT] response pipe: the simulator closed this thread's stream before it connected"
					          << " -- no answer will come" << std::endl;
					this->read_failed = true;
					return false;
				}
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
/* A pipe's write end polls POLLERR once the pipe has no reader left */
bool cvifstream::peerGone()
{
	if (this->peer_fd < 0)
		return false;
	struct pollfd p;
	p.fd = this->peer_fd;
	p.events = POLLOUT;
	p.revents = 0;
	return ::poll(&p, 1, 0) > 0 && (p.revents & POLLERR);
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
