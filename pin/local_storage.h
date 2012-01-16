#ifndef __LOCAL_STORAGE_H
#define __LOCAL_STORAGE_H

#define MAX_PIN_THREADS 2048

class Core;

struct ThreadLocalStorage
{
   Core* core;
   InstMode::inst_mode_t inst_mode;
   struct {
      int count;
      pthread_t thread_ptr;
      core_id_t tid;
   } pthread_create;
};
// Keep track of THREADID to Core* pointers (and some other stuff), way faster than a PinTLS lookup
extern std::vector<ThreadLocalStorage> localStore;

#endif // __LOCAL_STORAGE_H
