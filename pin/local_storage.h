#ifndef __LOCAL_STORAGE_H
#define __LOCAL_STORAGE_H

#define MAX_PIN_THREADS 2048

#include "inst_mode.h"
#include "spin_loop_detection.h"

#include <vector>

class Thread;
class Core;
class DynamicInstruction;

struct ThreadLocalStorage
{
   Thread* thread;
   DynamicInstruction* dynins;

   // Arguments passed to pthread_create and syscall(clone),
   // needed on return once we know the new thread's id
   struct
   {
      thread_id_t new_thread_id;
   } clone;
   // State used by spin loop detection
   SpinLoopDetectionState sld;
   // Malloc tracker
   struct
   {
      ADDRINT eip;
      size_t size;
   } malloc;
   ADDRINT lastCallSite;

   // Scratchpads for fault-injection
   static const unsigned int NUM_SCRATCHPADS = 3;
   static const size_t SCRATCHPAD_SIZE = 1024;
   char* scratch[NUM_SCRATCHPADS];
};
// Keep track of THREADID to Thread* pointers (and some other stuff), way faster than a PinTLS lookup
extern std::vector<ThreadLocalStorage> localStore;

#endif // __LOCAL_STORAGE_H
