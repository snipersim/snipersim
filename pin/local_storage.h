#ifndef __LOCAL_STORAGE_H
#define __LOCAL_STORAGE_H

#define MAX_PIN_THREADS 2048

#include "inst_mode.h"

#include <vector>

class Thread;
class Core;

struct ThreadLocalStorage
{
   Thread* thread;
   struct {
      int count;
      pthread_t thread_ptr;
      thread_id_t thread_id;
   } pthread_create;
};
// Keep track of THREADID to Thread* pointers (and some other stuff), way faster than a PinTLS lookup
extern std::vector<ThreadLocalStorage> localStore;

#endif // __LOCAL_STORAGE_H
