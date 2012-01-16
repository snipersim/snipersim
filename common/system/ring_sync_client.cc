#include "ring_sync_client.h"
#include "performance_model.h"
#include "log.h"

RingSyncClient::RingSyncClient(Core* core):
   _core(core),
   _elapsed_time(SubsecondTime::Zero()),
   _max_elapsed_time(SubsecondTime::Zero())
{}

RingSyncClient::~RingSyncClient()
{}

// Called by the core thread
void
RingSyncClient::synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*), void* abort_arg)
{
   LOG_ASSERT_ERROR(time == SubsecondTime::Zero(), "Time(%s), Cannot be used", itostr(time).c_str());

   _lock.acquire();

   _elapsed_time = _core->getPerformanceModel()->getElapsedTime();

   while (_elapsed_time >= _max_elapsed_time)
   {
      _cond.wait(_lock);
   }
   _lock.release();
}

// Called by the LCP
void
RingSyncClient::setMaxElapsedTime(SubsecondTime max_elapsed_time)
{
   assert (max_elapsed_time >= _max_elapsed_time);
   _max_elapsed_time = max_elapsed_time;

   if (_elapsed_time < _max_elapsed_time)
   {
      _cond.signal();
   }
}
