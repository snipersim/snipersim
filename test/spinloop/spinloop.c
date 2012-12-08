#include "sim_api.h"

#include <pthread.h>
#include <stdio.h>

volatile int shared = 0;
float sum = 0.;

void * work(void * id)
{
   for(int i = 0; i < 5; ++i)
   {
      printf("%d waiting\n", id);
      while(1)
      {
         // wait until the shared variable says free (0)
         while(shared != 0) ;
         // try to write our id, exit spin loop if successful
         if (__sync_bool_compare_and_swap(&shared, 0, id))
            break;
      }

      // do some work
      printf("%d working\n", id);
      for(int j = 0; j < 1000; ++j)
         sum += .1;

      // release lock
      shared = 0;
   }

   printf("%d done\n", id);
}

int main()
{
   SimRoiStart();

   pthread_t thread;
   pthread_create(&thread, NULL, work, 1);

   work(2);

   pthread_join(thread, NULL);

   SimRoiEnd();
}
