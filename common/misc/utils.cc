#include "utils.h"


/* ================================================================ */
/* Utility function definitions */
/* ================================================================ */

String myDecStr(UInt64 v, UInt32 w)
{
   std::ostringstream o;
   o.width(w);
   o << v;
   String str(o.str().c_str());
   return str;
}


bool isPower2(UInt32 n)
{ return ((n & (n - 1)) == 0); }


SInt32 floorLog2(UInt32 n)
{
   SInt32 p = 0;

   if (n == 0) return -1;

   if (n & 0xffff0000) { p += 16; n >>= 16; }
   if (n & 0x0000ff00) { p +=  8; n >>=  8; }
   if (n & 0x000000f0) { p +=  4; n >>=  4; }
   if (n & 0x0000000c) { p +=  2; n >>=  2; }
   if (n & 0x00000002) { p +=  1; }

   return p;
}


SInt32 ceilLog2(UInt32 n)
{ return floorLog2(n - 1) + 1; }


// http://stackoverflow.com/a/6998789/199554

UInt64 countBits(UInt64 n)
{
   UInt64 result = 0;

   if(n == 0)
      return 0;

   for(result = 1; n &= n - 1; ++result)
      ;

   return result;
}
