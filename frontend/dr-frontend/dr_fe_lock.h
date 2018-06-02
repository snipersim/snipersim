#ifndef _DR_FE_LOCK_H_
#define _DR_FE_LOCK_H_

namespace frontend
{
/**
 * @class FELock<DRFrontend>
 * @brief 
 * Lock specialized for DynamoRIO
 */

template <> class FELock<DRFrontend> : public LockBase<DRFrontend>
{
  public:
    FELock();
    ~FELock();
    void acquire_lock(threadid_t tid);
    void release_lock();
  private:
    void * the_lock;
    
};

} // end namespace frontend

#include "dr_fe_lock.tcc"

#endif // _DR_FE_LOCK_H_