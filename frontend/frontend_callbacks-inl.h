namespace frontend
{

template <typename T> inline FrontendCallbacks<T>::FrontendCallbacks(FrontendOptions<T>* opts, FrontendControl<T>* cntrl, thread_data_t* td)
{
  m_options = opts;
  m_control = cntrl;
  m_thread_data = td;
}

template <typename T> inline FrontendCallbacks<T>::~FrontendCallbacks()
{
  delete this->m_options;
}


template <typename T> inline void FrontendCallbacks<T>::__sendInstructionSpecialized(threadid_t threadid, uint32_t num_addresses, bool isbefore)
{
}

} // namespace frontend
