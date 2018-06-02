#ifndef _DR_FE_COPY_H_
#define _DR_FE_COPY_H_

#include "frontend_utils.h"

// Forward declaration to avoid circular dependencies
class DRFrontend;

namespace frontend
{

/**
 * @class FECopy
 * @brief 
 * Specialized memcopy for the DynamoRIO tool
 */

template <> class FECopy<DRFrontend>
{
  public:
    FECopy();
    ~FECopy();
    void copy_to_memory(uint8_t* data_buffer, uint64_t d_addr, uint32_t data_size);
    void copy_from_memory(uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size);
    static void __DR_safeCopy(void* dst, const void* src, uint32_t size);
    
}; 
 
} // end namespace frontend

#include "dr_fe_copy.tcc"

#endif // _DR_FE_COPY_H_