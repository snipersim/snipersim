namespace frontend
{

template <typename T> inline FELock<T>::FELock()
{
//  the_lock = PTHREAD_MUTEX_INITIALIZER;
}

template <typename T> inline FELock<T>::~FELock()
{
}

template <typename T> inline void FELock<T>::acquire_lock(threadid_t tid)
{
//  pthread_mutex_lock(&the_lock);
}

template <typename T> inline void FELock<T>::release_lock()
{ 
//  pthread_mutex_unlock(&the_lock);
}

template <typename T> inline FECopy<T>::FECopy()
{
}

template <typename T> inline FECopy<T>::~FECopy()
{
}

template <typename T> inline void FECopy<T>::copy_to_memory(uint8_t* data_buffer, uint64_t d_addr, uint32_t data_size)
{
  std::memcpy(reinterpret_cast<void*>(d_addr), data_buffer, data_size);
}

template <typename T> inline void FECopy<T>::copy_from_memory(uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size)
{
  std::memcpy(data_buffer, reinterpret_cast<void*>(d_addr), data_size);
}

} // namespace frontend
