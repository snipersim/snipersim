#ifndef MEMGUARD_H
#define MEMGUARD_H

// Class that allocates a protected page. Any code writing to random memory locations
// hopefully hits one of our pages, which will cause the process to receive a SEGFAULT.
// A simple GDB backtrace should then point to the offender.

#define PAGE_SIZE 4096

class MemGuard {
   private:
      char m_guard[2*PAGE_SIZE];

      void protect();
      void unprotect();
      bool isAligned();
      void* pagePointer();

   public:
      MemGuard();
      MemGuard(const MemGuard& m);
      MemGuard& operator= (const MemGuard& m);
      ~MemGuard();
};

#endif //MEMGUARD_H
