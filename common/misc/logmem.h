#ifndef LOGMEM_H
#define LOGMEM_H

// Define to have a list of unfreed memory allocations written to allocations.out
//#define LOGMEM_ENABLED

void logmem_enable(bool enabled);
void logmem_write_allocations();

#endif // LOGMEM_H
