#ifndef __PROGRESS_H
#define __PROGRESS_H

#include "subsecond_time.h"

class Progress {
   public:
      static void init(void);
      static void fini(void);
   private:
      static SInt64 record(UInt64 init, UInt64 time);
};

#endif // __PROGRESS_H
