#include "sift_utils.h"
#include "sift_format.h"

#include <cstdio>

void Sift::hexdump(const void * data, uint32_t size)
{
   printf("(%d) ", size);
   for(unsigned int i = 0; i < size; ++i)
     printf("%02x ", ((unsigned char *)data)[i]);
   printf("\n");
}
