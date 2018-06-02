#include "dr_api.h"
#include "drutil.h"

namespace frontend
{
  
FrontendOptions<DRFrontend>::FrontendOptions(int argc, const char* argv[])
{
  this->parsing_error = true;
  this->current_mode = Sift::ModeIcount;
  
  // Parse the command line
  std::string parse_err;
  this->parsing_error = !droption_parser_t::parse_argv(DROPTION_SCOPE_CLIENT, argc, argv, &parse_err, NULL);
  if (this->parsing_error) {
    dr_fprintf(STDERR, "Usage error: %s", parse_err.c_str());
    dr_fprintf(STDERR, "Usage: dr-frontend [options]\n%s", cmd_summary().c_str());
    dr_abort();
  } 
  
  this->verbose = DRVerbose.get_value();
  this->use_roi = DRUseROI.get_value();
  this->mpi_implicit_roi = DRMPIImplicitROI.get_value();
  this->fast_forward_target = DRFastForwardTarget.get_value();
  detailed_target = DRDetailedTarget.get_value();
  blocksize = DRBlocksize.get_value();
  output_file = DROutputFile.get_value();
  emulate_syscalls = DREmulateSyscalls.get_value();
  response_files = DRUseResponseFiles.get_value();
  send_physical_address = DRSendPhysicalAddresses.get_value();
  stop_address = DRStopAddress.get_value();
  app_id = DRSiftAppId.get_value();
  flow_control = DRFlowControl.get_value();
  flow_control_ff = DRFlowControlFF.get_value();
  this->ssh = DRSSH.get_value();
}


inline std::string FrontendOptions<DRFrontend>::cmd_summary()
{
  return droption_parser_t::usage_short(DROPTION_SCOPE_ALL);
}

} // end namespace frontend