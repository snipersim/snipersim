#include "../common/user/thread_support.h"

#include <stdio.h>
#include <pthread.h>

void CarbonGetThreadToSpawn(ThreadSpawnRequest *req);
void *CarbonSpawnManagedThread(void *p);
void *CarbonThreadSpawner(void *p);
int CarbonSpawnThreadSpawner();
void CarbonDequeueThreadSpawnReq (ThreadSpawnRequest *req);
void CarbonPthreadAttrInitOtherAttr(pthread_attr_t *attr);

void *CarbonSpawnManagedThread(void * arg)
{
   ThreadSpawnRequest thread_req;

   CarbonDequeueThreadSpawnReq (&thread_req);

   thread_req.func(thread_req.arg);

   return NULL;
}

// This function spawns the thread spawner
int CarbonSpawnThreadSpawner()
{
   setvbuf( stdout, NULL, _IONBF, 0 );

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

   CarbonPthreadAttrInitOtherAttr(&attr);

   pthread_create(&thread, &attr, CarbonThreadSpawner, NULL);

   return 0;
}

// This function will spawn threads provided by the sim
void *CarbonThreadSpawner(void * arg)
{
   while(1)
   {
      ThreadSpawnRequest req;

      // Wait for a spawn request
      CarbonGetThreadToSpawn(&req);

      if(req.func)
      {
         pthread_t thread;
         pthread_attr_t attr;
         pthread_attr_init(&attr);
         pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

         CarbonPthreadAttrInitOtherAttr(&attr);

         pthread_create(&thread, &attr, CarbonSpawnManagedThread, NULL);
      }
      else
      {
         break;
      }
   }

   return NULL;
}

// Gets replaced while running with Pin
// attribute 'noinline' necessary to make the scheme work correctly with
// optimizations enabled; asm ("") in the body prevents the function from being
// optimized away
__attribute__((noinline)) void CarbonGetThreadToSpawn(ThreadSpawnRequest *req)
{
   asm ("");
}

__attribute__((noinline)) void CarbonDequeueThreadSpawnReq(ThreadSpawnRequest *req)
{
   asm ("");
}

__attribute__((noinline)) void CarbonPthreadAttrInitOtherAttr(pthread_attr_t *attr)
{
   asm ("");
}
