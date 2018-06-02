#ifndef _DR_FE_OPTIONS_H_
#define _DR_FE_OPTIONS_H_

#include "droption.h"
#include "frontend_options.h"

class DRFrontend;

static droption_t<std::string> DROutputFile
(DROPTION_SCOPE_CLIENT, "o", "trace", "output",
 "Name of the output file to save the trace.");
static droption_t<unsigned int> DRBlocksize
(DROPTION_SCOPE_CLIENT, "b", 0, "blocksize",
 "If >0, several traces are created, each with <blocksize> instructions.");
static droption_t<unsigned int> DRUseROI
(DROPTION_SCOPE_CLIENT, "roi", 0, "Use ROI markers",
 "If not 0, only marked Regions Of Interest will be simulated in detail.");
static droption_t<unsigned int> DRMPIImplicitROI
(DROPTION_SCOPE_CLIENT, "roi-mpi", 0, "Use MPI ROI markers",
 "Implicit ROI between MPI_Init and MPI_Finalize.");
static droption_t<unsigned int> DRFastForwardTarget
(DROPTION_SCOPE_CLIENT, "f", 0, "instructions to fast forward",
 "Number of instructions to fast forward at the beginning of the simulation.");
static droption_t<unsigned int> DRDetailedTarget
(DROPTION_SCOPE_CLIENT, "d", 0, "instructions to trace in detail",
 "Number of instructions to trace in detail (default = all).");
static droption_t<unsigned int> DRUseResponseFiles
(DROPTION_SCOPE_CLIENT, "r", 0, "use response files",
 "Use response files (required for multithreaded applications or when emulating syscalls, default = 0)");
static droption_t<unsigned int> DREmulateSyscalls
(DROPTION_SCOPE_CLIENT, "e", 0, "emulate syscalls",
 "Emulate syscalls (required for multithreaded applications, default = 0)");
static droption_t<bool> DRSendPhysicalAddresses
(DROPTION_SCOPE_CLIENT, "pa", false, "send logical to physical address mapping",
 "Send logical to physical address mapping.");
static droption_t<unsigned int> DRFlowControl
(DROPTION_SCOPE_CLIENT, "flow", 1000, "instructions before sync",
 "Number of instructions to send before syncing up.");
static droption_t<unsigned int> DRFlowControlFF
(DROPTION_SCOPE_CLIENT, "flowff", 100000, "instructions before sync in fast-forward mode",
 "Number of instructions to batch up before sending instruction counts in fast-forward mode.");
static droption_t<int> DRSiftAppId
(DROPTION_SCOPE_CLIENT, "s", 0, "sift app id",
 "Sift app id (default = 0).");
static droption_t<bool> DRRoutineTracing
(DROPTION_SCOPE_CLIENT, "rtntrace", false, "routine tracing",
 "Activates routine tracing.");
static droption_t<bool> DRRoutineTracingOutsideDetailed
(DROPTION_SCOPE_CLIENT, "rtntrace_outsidedetail", false, "routine tracing",
 "Activates routine tracing outside detailed simulation.");
static droption_t<bool> DRDebug
(DROPTION_SCOPE_CLIENT, "debug", false, "start debugger on internal exception",
 "Start debugger on internal exception.");
static droption_t<bool> DRVerbose
(DROPTION_SCOPE_CLIENT, "verbose", false, "verbose output",
 "Activates verbose output.");
static droption_t<unsigned int> DRStopAddress
(DROPTION_SCOPE_CLIENT, "stop", 0, "stop address",
 "Stop address (0 = disabled).");
static droption_t<bool> DRSSH
(DROPTION_SCOPE_CLIENT, "ssh", false, "frontend and backend connected by network",
 "Backend and frontend communicate over the network.");

namespace frontend
{

/**
* @class FrontendOptions<DRFrontend>
*
* Template specialization of FrontedOptions for the command line options with DynamoRIO.
* Common members from OptionsBase class are accessible.
* Members specific to DynamoRIO frontend are defined here.
*/

template <> class FrontendOptions<DRFrontend> : public OptionsBase<DRFrontend>
{
  public:
  /// Constructor
  /// Parses the command line options, saved in the fields here
  /// Saves status of parsing in m_success
  FrontendOptions(int argc, const char* argv[]);

  /// Destructor
  ~FrontendOptions();

  /// Return a string with the available command line options
  std::string cmd_summary();

};

} // end namespace frontend

#include "dr_fe_options.tcc"

#endif // _DR_FE_OPTIONS_H_