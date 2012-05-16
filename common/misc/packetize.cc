#include "packetize.h"

UnstructuredBuffer::UnstructuredBuffer()
{
}

const void* UnstructuredBuffer::getBuffer()
{
   return m_chars.data();
}

void UnstructuredBuffer::clear()
{
   m_chars.erase();
}

int UnstructuredBuffer::size()
{
   return m_chars.size();
}

// put buffer

UnstructuredBuffer& UnstructuredBuffer::operator<<(std::pair<const void*, int> buffer)
{
   const void *buff = buffer.first;
   int size = buffer.second;

   assert(size >= 0);
   put<UInt8>((UInt8*) buff, size);
   return *this;
}

// get buffer

UnstructuredBuffer& UnstructuredBuffer::operator>>(std::pair<void*, int> buffer)
{
   const void *buff = buffer.first;
   __attribute__((unused)) int size = buffer.second;

   assert(size >= 0);
   __attribute__((unused)) bool res = get<UInt8>((UInt8*) buff, size);
   assert(res == true);
   return *this;
}

#ifdef DEBUG_UNSTRUCTURED_BUFFER

int main(int argc, char* argv[])
{
   UnstructuredBuffer buff;
   UInt32 data1 = 1;
   UInt8 data2 = 'a';
   UInt32 data3 = 3;
   buff << data1 << data2 << data3;

   UInt32 data1prime;
   UInt8 data2prime;
   UInt32 data3prime;
   buff >> data1prime >> data2prime >> data3prime;

   cout << data1prime << " " << data2prime << " " << data3prime << endl;
   assert(data1 == data1prime);
   assert(data2 == data2prime);
   assert(data3 == data3prime);
   cout << "success: test 1" << endl;

   char data4[] = "hello world";
   char data4prime[] = "aaaaaaaaaaaa";
   buff << make_pair(data4, 12);
   buff >> make_pair(data4prime, 12);


   cout << data4prime << endl;
   assert(strcmp(data4prime, data4) == 0);
   cout << "success: test 2" << endl;

   int data5 = 42;
   buff << data5;
   int data5prime = -1;
   buff >> data5prime;

   cout << data5prime << endl;
   assert(data5 == data5prime);
   cout << "success: test 3" << endl;

   UInt64 data6 = 40;
   UInt64 data7 = 41;
   buff << data6 << data7;
   UInt64 data6prime = 0;
   UInt64 data7prime = 0;
   buff >> data6prime >> data7prime;

   cout << data6prime << " " << data7prime << endl;
   assert(data6 == data6prime);
   assert(data7 == data7prime);
   cout << "success: test 4" << endl;

   cout << "All tests passed" << endl;

   return 0;
}

#endif
