#ifndef LOCKFREE_HASH_H
#define LOCKFREE_HASH_H

#include <map>
#include <utility>
#include <iostream>
#include <assert.h>
#include "basic_hash.h"
#include "fixed_types.h"

//#define DEBUG_LOCKFREE_HASH


class LockFreeHash : private BasicHash
{
   private:
      UInt64 bucket_size(UInt64 key);

   public:
      LockFreeHash(UInt64 size);
      ~LockFreeHash();

      std::pair<bool, UInt64> find(UInt64 key);
      bool insert(UInt64 key, UInt64 value);
};

#endif
