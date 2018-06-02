namespace frontend
{

template <typename T> inline bool OptionsBase<T>::parse_cmd_status()
{
  return parsing_error;
}

template <typename T> inline uint64_t OptionsBase<T>::get_fast_forward_target()
{
  return fast_forward_target;
}

template <typename T> inline uint64_t OptionsBase<T>::get_flow_control()
{
  return flow_control;
}

template <typename T> inline uint64_t OptionsBase<T>::get_flow_control_ff()
{
  return flow_control_ff;
}

template <typename T> inline uint64_t OptionsBase<T>::get_blocksize()
{
  return blocksize;
}

template <typename T> inline bool OptionsBase<T>::get_verbose()
{
  return verbose;
}

template <typename T> inline bool OptionsBase<T>::get_use_roi()
{
  return use_roi;
}

template <typename T> inline bool OptionsBase<T>::get_mpi_implicit_roi()
{
  return mpi_implicit_roi;
}

template <typename T> inline bool OptionsBase<T>::get_emulate_syscalls()
{
  return emulate_syscalls;
}

template <typename T> inline bool OptionsBase<T>::get_response_files()
{
  return response_files;
}

template <typename T> inline bool OptionsBase<T>::get_send_physical_address()
{
  return send_physical_address;
}

template <typename T> inline uint64_t OptionsBase<T>::get_stop_address()
{
  return stop_address;
}

template <typename T> inline Sift::Mode OptionsBase<T>::get_current_mode()
{
  return current_mode;
}

template <typename T> inline void OptionsBase<T>::set_current_mode(Sift::Mode mode)
{
  current_mode = mode;
}

template <typename T> inline uint32_t OptionsBase<T>::get_app_id()
{
  return app_id;
}

template <typename T> inline void OptionsBase<T>::set_app_id(uint32_t appId)
{
  app_id = appId;
}

template <typename T> inline std::string OptionsBase<T>::get_output_file()
{
  return output_file;
}

template <typename T> inline FrontendOptions<T>::~FrontendOptions()
{
}

template <typename T> inline std::string FrontendOptions<T>::cmd_summary()
{
  std::string usage;
  this->opt.getUsage(usage, 80, ez::ezOptionParser::ALIGN);
  return usage;
}

template <typename T> inline uint32_t FrontendOptions<T>::get_ncores()
{
  return ncores;
}

template <typename T> inline std::string FrontendOptions<T>::get_statefile()
{
  return statefile;
}

template <typename T> inline std::string FrontendOptions<T>::get_cmd_app()
{
  return cmd_app;
}

template <typename T> inline FrontendISA FrontendOptions<T>::get_theISA()
{
  return theISA;
}

template <typename T> inline bool OptionsBase<T>::get_ssh()
{
  return ssh;
}

} // namespace frontend
