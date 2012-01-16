#include "checksum.h"
#include "log.h"

UInt64 computeCheckSum(const Byte* buffer, UInt32 length)
{
   UInt64 checksum = 0;
   for (UInt32 i = 0; i < length; i++)
   {
      checksum += (UInt64) buffer[i];
   }
   return checksum;
}
