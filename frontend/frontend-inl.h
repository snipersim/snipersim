namespace frontend
{

template <typename T> inline Frontend<T>::Frontend(FrontendISA theISA)
: num_threads(0)
{
}

template <typename T> inline Frontend<T>::Frontend()
: num_threads(0)  // base ctor is always called if it has no arguments
{
}

template <typename T> inline Frontend<T>::~Frontend()
{
  tidptrs.clear();
  m_sysmodel.reset();
  std::cerr << "Deleting new_threadid lock in FRONTEND" << std::endl;
  delete this->new_threadid_lock; 
  delete this->m_control;
  delete this->m_callbacks;
  delete this->m_threads;
}  

template <typename T> inline thread_data_t* Frontend<T>::get_thread_data()
{
  return m_thread_data;
}

template <typename T> inline FrontendControl<T>* Frontend<T>::get_control()
{
  return m_control;
}

template <typename T> inline ExecFrontend<T>::~ExecFrontend()
{

}

} // namespace frontend