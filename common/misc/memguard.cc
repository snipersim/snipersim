#include "memguard.h"

#include <sys/mman.h>

MemGuard::MemGuard()
{
   protect();
}

MemGuard::MemGuard(const MemGuard& m)
{
   protect();
   // Explicit copy constructor: don't access m->guard
}

MemGuard& MemGuard::operator= (const MemGuard& m)
{
   // Explicit operator=: don't access m->guard
   return *this;
}

MemGuard::~MemGuard() {
   // Memory may be reused by actual data, so reset the permissions
   unprotect();
}

void MemGuard::protect()
{
   mprotect(pagePointer(), isAligned() ? 2*PAGE_SIZE : PAGE_SIZE, PROT_NONE);
}

void MemGuard::unprotect()
{
   mprotect(pagePointer(), isAligned() ? 2*PAGE_SIZE : PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
}

bool MemGuard::isAligned()
{
   return ((unsigned long)this->m_guard & (PAGE_SIZE-1)) == 0;
}

void* MemGuard::pagePointer()
{
   return (void*)((unsigned long)(this->m_guard + PAGE_SIZE - 1) & ~(PAGE_SIZE-1));
}
