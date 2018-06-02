namespace frontend
{

template <typename T> 
inline 
FrontendSyscallModelBase<T>::FrontendSyscallModelBase
(FrontendOptions<T>* opts, thread_data_t* td, FELock<T>* ntid_lock, std::deque<uint64_t>* tids)
{
  m_options = opts;
  m_thread_data = td;
  m_new_threadid_lock = ntid_lock;
  m_access_memory_lock = new FELock<T>;
  tidptrs = tids;
}

template <typename T> inline FrontendSyscallModelBase<T>::~FrontendSyscallModelBase()
{
  delete this->m_access_memory_lock;
  delete this->m_new_threadid_lock;
}

} // namespace frontend
