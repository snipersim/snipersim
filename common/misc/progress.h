#ifndef __PROGRESS_H
#define __PROGRESS_H

#include "subsecond_time.h"

class Progress {
   public:
      static void init(void);
      static void fini(void);
   private:
      static void record(bool init, subsecond_time_t time);
};

#endif // __PROGRESS_H
