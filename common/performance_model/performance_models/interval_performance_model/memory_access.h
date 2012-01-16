#ifndef __MEMORY_ACCESS_H
#define __MEMORY_ACCESS_H

namespace Memory
{
   struct Access
   {
      union {
         UInt64 address, virt, phys;
      };
      //static const UInt64 length = 8;
      void set(UInt64 address)
      {
         this->address = address;
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
