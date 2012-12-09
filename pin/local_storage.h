#ifndef __LOCAL_STORAGE_H
#define __LOCAL_STORAGE_H

#define MAX_PIN_THREADS 2048

#include "inst_mode.h"
#include "spin_loop_detection.h"

#include <vector>

class Thread;
class Core;

struct ThreadLocalStorage
{
   Thread* thread;
   // Arguments passed to pthread_create and syscall(clone),
   // needed on return once we know the new thread's id
   struct
   {
      int count;
      pthread_t thread_ptr;
      thread_id_t thread_id;
      void* tid_ptr;
      bool clear_tid;
   } pthread_create;
   // State used by spin loop detection
   SpinLoopDetectionState sld;
};
// Keep track of THREADID to Thread* pointers (and some other stuff), way faster than a PinTLS lookup
extern std::vector<ThreadLocalStorage> localStore;

#endif // __LOCAL_STORAGE_H
