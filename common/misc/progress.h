#ifndef __PROGRESS_H
#define __PROGRESS_H

#include "subsecond_time.h"

class Progress
{
   public:
      Progress();
      ~Progress();

      void setProgress(float progress);

   private:
      static SInt64 __record(UInt64 arg, UInt64 val)
      { ((Progress *)arg)->record(val); return 0; }
      void record(UInt64 time);

      bool m_enabled;
      FILE * m_fp;
      time_t m_t_last;
      static const time_t m_interval = 2;

      bool m_manual;
      float m_manual_value;
};

#endif // __PROGRESS_H
