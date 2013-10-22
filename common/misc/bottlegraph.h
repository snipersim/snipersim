#ifndef __BOTTLEGRAPH_H
#define __BOTTLEGRAPH_H

#include "fixed_types.h"
#include "subsecond_time.h"

#include <vector>

class BottleGraphManager
{
   public:
      BottleGraphManager(int max_threads);

      void threadStart(thread_id_t thread_id);
      void update(SubsecondTime time, thread_id_t thread_id, bool running);

   private:
      SubsecondTime m_time_last;
      std::vector<bool> m_running;
      std::vector<SubsecondTime> m_contrib;
      std::vector<SubsecondTime> m_runtime;
};

#endif // __BOTTLEGRAPH_H
