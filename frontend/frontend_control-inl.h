namespace frontend
{

template <typename T> inline FrontendControl<T>::FrontendControl(FrontendOptions<T>* opts, thread_data_t* td, rombauts::shared_ptr<FrontendSyscallModel<T>> sysmodel)
: in_roi(false)
, any_thread_in_detail(false)
{
  m_options = opts;
  m_thread_data = td;
  m_sysmodel = sysmodel;
}

template <typename T> inline FrontendControl<T>::~FrontendControl()
{
  m_sysmodel.reset(); // decrements/deletes shared_ptr
}

template <typename T> inline void FrontendControl<T>::removeInstrumentation()
{
}

template <typename T> inline void FrontendControl<T>::free_thread_data(size_t thread_data_size)
{
  free(m_thread_data);
}

template <typename T> inline void FrontendControl<T>::set_in_roi(bool ir)
{
  in_roi = ir;
}

template <typename T> inline bool FrontendControl<T>::get_in_roi()
{
  return in_roi;
}

template <typename T> inline bool FrontendControl<T>::get_any_thread_in_detail()
{
  return any_thread_in_detail;
}

template <typename T> inline void FrontendControl<T>::set_any_thread_in_detail(bool in_detail)
{
  any_thread_in_detail = in_detail;
}

template <typename T> inline uint64_t FrontendControl<T>::get_stop_address()
{
  return m_options->get_stop_address();
}

} // namespace frontend
