namespace frontend
{
  
template <> void FrontendThreads<DRFrontend>::callFinishHelper(threadid_t threadid)
{
// bool success = dr_create_client_thread(threadFinishHelper, (void*)(unsigned long)threadid);
//  DR_ASSERT(success);
//  dr_event_wait(child_dead);
//  dr_sleep(30);
  threadFinishHelper((void*)(unsigned long)threadid);
}

} // end namespace frontend