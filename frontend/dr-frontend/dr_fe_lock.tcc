namespace frontend
{

inline FELock<DRFrontend>::FELock()
{
  this->the_lock = dr_mutex_create();
  std::cerr << "Lock created" << std::endl;
}

inline FELock<DRFrontend>::~FELock()
{
    std::cerr << "Going to destroy Lock" << std::endl;
  dr_mutex_destroy(this->the_lock);
      std::cerr << "Lock destroyed" << std::endl;

}

inline void FELock<DRFrontend>::acquire_lock(threadid_t tid)
{
  dr_mutex_lock(this->the_lock);
    std::cerr << "Lock acquired" << std::endl;

}

inline void FELock<DRFrontend>::release_lock()
{ 
  dr_mutex_unlock(this->the_lock);
    std::cerr << "Lock released" << std::endl;

}

} // end namespace frontend