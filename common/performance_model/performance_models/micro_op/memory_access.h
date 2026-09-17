#ifndef __MEMORY_ACCESS_H
#define __MEMORY_ACCESS_H

namespace Memory
{
   struct Access
   {
      union {
         UInt64 address, virt, phys;
      };
      /** Bytes accessed, when known dynamically (vector and matrix-tile
          accesses); 0 means the micro-op's static size applies. */
      UInt32 size;
      void set(UInt64 address, UInt32 size = 0)
      {
         this->address = address;
         this->size = size;
      }
   };

   // Original interface, with virt/phys/length as separate fields:
   //inline Access make_access(UInt64 virt, UInt64 phys, UInt64 length = 8);

   inline Access make_access(UInt64 address)
   {
      Access access;
      access.set(address);
      return access;
   }
}

#endif // __MEMORY_ACCESS_H
