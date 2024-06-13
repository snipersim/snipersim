#include "zfstream.h"

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
cvifstream::cvifstream(const char * filename, std::ios_base::openmode mode)
{
	std::string mode_str;
	if(mode & std::ios_base::in)
		mode_str += "r";
	if(mode & std::ios_base::out)
		mode_str += "w";
	if(mode & std::ios_base::binary)
		mode_str += "b";

	// FIXME: Reimplement this correctly
	if(mode & (std::ios_base::app | std::ios_base::ate | std::ios_base::trunc))
		assert(false);

	this->stream = std::fopen(filename, mode_str.c_str());
	assert(this->stream != NULL);
	this->buffer_in_use = false;
}

cvifstream::~cvifstream()
{
	std::fclose(this->stream);
}

void cvifstream::read(char* s, std::streamsize n)
{
	size_t nr_to_read = n;
	char* start_buffer = s;
	if(this->buffer_in_use)
    {
		start_buffer[0] = this->peek_buffer;
		nr_to_read--;
		start_buffer++;
		this->buffer_in_use = false;
	}
	if(nr_to_read > 0)
    {
		ssize_t num_read = std::fread(start_buffer, sizeof(char), nr_to_read, this->stream);
		assert(num_read == n || std::ferror(this->stream) == 0);
	}
}

int cvifstream::peek()
{
	if(!this->buffer_in_use)
    {
		this->read(&this->peek_buffer, 1);
		this->buffer_in_use = true;
	}
	return this->peek_buffer;
}

bool cvifstream::fail() const 
{
	return this->stream == NULL;
}
