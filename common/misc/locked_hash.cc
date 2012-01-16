#include "locked_hash.h"

LockedHash::LockedHash(UInt64 size)
      :
      _size(size),
      _bins(new Bucket[size]),
      _locks(new Lock[size])
{
}

LockedHash::~LockedHash()
{
   // FIXME: For some reason, this seg faults. Only deleted during
   // shutdown, so maybe this OK? But still a huge hack.

   // delete [] _bins;
   // delete [] _locks;
}

std::pair<bool, UInt64> LockedHash::find(UInt64 key)
{
   UInt64 index = key % _size;
   std::pair<bool, UInt64> res;

   res.first = false;

   _locks[index].acquire();

   std::unordered_map<UInt64,UInt64>::iterator iter = _bins[index].find(key);
   if (iter != _bins[index].end())
   {
      res.first = true;
      res.second = iter->second;
   }

   _locks[index].release();
   return res;
}

void LockedHash::remove(UInt64 key)
{
   UInt64 index = key % _size;
   _locks[index].acquire();

   std::unordered_map<UInt64,UInt64>::iterator iter = _bins[index].find(key);
   if (iter != _bins[index].end())
   {
       _bins[index].erase(iter);
   }

   _locks[index].release();
}

bool LockedHash::insert(UInt64 key, UInt64 value)
{
   UInt64 index = key % _size;
   _locks[index].acquire();
   _bins[index].insert(std::make_pair(key, value));
   _locks[index].release();

   return true;
}


#ifdef DEBUG_LOCKED_HASH

int main(int argc, char* argv[])
{
   LockedHash hash(100);
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
