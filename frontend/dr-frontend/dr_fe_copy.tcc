#include "dr_api.h"

namespace frontend
{

// local function to perform a safe copy between memory addresses
void FECopy<DRFrontend>::__DR_safeCopy(void* dst, const void* src, uint32_t size)
{
  // We can't copy between memory locations in a safe way with DR API, it has to be done in 2 phases: read and write
  size_t read_bytes, written_bytes;
  void * tmp_buffer = dr_custom_alloc(NULL, (dr_alloc_flags_t) 0, size, 0, NULL);
  DR_ASSERT(tmp_buffer != NULL);
 
  // read to temporary buffer
  DR_ASSERT(dr_safe_read(src, size, tmp_buffer, &read_bytes));
  // write to dest addr
  DR_ASSERT(dr_safe_write(dst, size, tmp_buffer, &written_bytes)); 

  // free the temporary buffer
  DR_ASSERT(dr_custom_free(NULL, (dr_alloc_flags_t) 0, tmp_buffer, size));

}

inline FECopy<DRFrontend>::FECopy()
{
}

inline FECopy<DRFrontend>::~FECopy()
{
}

inline void FECopy<DRFrontend>::copy_to_memory(uint8_t* data_buffer, uint64_t d_addr, uint32_t data_size)
{
  __DR_safeCopy(reinterpret_cast<void*>(d_addr), data_buffer, data_size);
}

inline void FECopy<DRFrontend>::copy_from_memory(uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size)
{
  __DR_safeCopy(data_buffer, reinterpret_cast<void*>(d_addr), data_size);
}
 
} // end namespace frontend