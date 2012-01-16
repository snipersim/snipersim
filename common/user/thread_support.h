#ifndef THREAD_SUPPORT_H
#define THREAD_SUPPORT_H

#include "fixed_types.h"
#include "subsecond_time_c.h"

typedef void *(*thread_func_t)(void *);

typedef struct
{
   SInt32 msg_type;
   thread_func_t func;
   void *arg;
   SInt32 requester;
   core_id_t core_id;
   subsecond_time_t time;
} ThreadSpawnRequest;

typedef struct
{
   SInt32 msg_type;
   SInt32 sender;
   core_id_t core_id;
} ThreadJoinRequest;

#endif
