namespace frontend
{
  
template <> void FrontendControl<DRFrontend>::free_thread_data(size_t thread_data_size)
{
  std::cerr << "Going to free" << std::endl;
  dr_custom_free(NULL, (dr_alloc_flags_t) 0, m_thread_data, thread_data_size);
  std::cerr << "Thread data freed" << std::endl;
}

template <> void FrontendControl<DRFrontend>::getCode(uint8_t* dst, const uint8_t* src, uint32_t size)
{
  FECopy<DRFrontend>::__DR_safeCopy(dst, src, size);
}

} // end namespace frontend