#ifndef _FRONTEND_UTILS_H_
#define _FRONTEND_UTILS_H_

//#include <pthread.h>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "sift_format.h"
#include "frontend_defs.h"

namespace frontend
{

/**
 * @class LockBase
 * @brief 
 * Base of the lock interface that can be specialized in specific frontends
 */

template <typename T> class LockBase
{
  public:
    void acquire_lock(threadid_t tid);
    void release_lock();
    
};

/**
 * @class FELock
 * @brief 
 * Frontend lock interface, general to any frontend implementation
 */

template <typename T> class FELock : public LockBase<T>
{
  public:
    FELock();
    ~FELock();
    void acquire_lock(threadid_t tid);
    void release_lock();
  private:
    //pthread_mutex_t the_lock;
    
};

/**
 * @class FECopy
 * @brief 
 * Frontend memcopy interface, to be specialized by each frontend implementation
 */

template <typename T> class FECopy
{
  public:
    FECopy();
    ~FECopy();
    void copy_to_memory(uint8_t* data_buffer, uint64_t d_addr, uint32_t data_size);
    void copy_from_memory(uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size);
    
};

} // namespace frontend

#include "frontend_utils-inl.h"

#endif // _FRONTEND_UTILS_H_
