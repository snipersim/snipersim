#include "lockfree_hash.h"

LockFreeHash::LockFreeHash(UInt64 size) : BasicHash::BasicHash(size)
{
}

LockFreeHash::~LockFreeHash()
{
}


UInt64 LockFreeHash::bucket_size(UInt64 key)
{
   UInt64 index = key % size;
   Bucket& bucket = array[index];
   return bucket.size();
}

std::pair<bool, UInt64> LockFreeHash::find(UInt64 key)
{
   std::pair<bool, UInt64> res = BasicHash::find(key);
   assert(bucket_size(key) <= 1);

   return res;
}

bool LockFreeHash::insert(UInt64 key, UInt64 value)
{
   bool res = BasicHash::insert(key, value);
   assert(bucket_size(key) <= 1);

   return res;
}


#ifdef DEBUG_LOCKFREE_HASH

int main(int argc, char* argv[])
{
   LockFreeHash hash(100);
   UInt64 ids[4] = {1001, 1050, 1011, 1099};

   for (int i = 0; i < 4; i++)
      hash.insert(ids[i], i);

   for (int i = 3; i >= 0; i--)
      assert(hash.find(ids[i]).first == true);
   cerr << "Test 1 passed" << endl;


   cerr << "Test 2 should fail in assertion" << endl;
   ids[3] = ids[0] + 100;

   for (int i = 0; i < 4; i++)
      hash.insert(ids[i], i);

   cerr << "All tests passed" << endl;

   return 0;
}


#endif
