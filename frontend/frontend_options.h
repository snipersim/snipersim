#ifndef _FRONTEND_OPTIONS_H_
#define _FRONTEND_OPTIONS_H_

#include "sift_format.h"
#include <string>
#include "ezOptionParser.hpp"
#include "frontend_defs.h"

namespace frontend
{

/**
 * @class OptionsBase
 *
 * Base template class for the frontend command line options.
 * All the generic members to any frontend must be placed here.
 */

template <typename T> class OptionsBase
{
  public:
  /// Returns true if there has been an error with the parsing
  bool parse_cmd_status();

  /// Get protected fields
  uint64_t get_fast_forward_target();
  uint64_t get_blocksize();
  bool get_verbose();
  bool get_use_roi();  
  bool get_mpi_implicit_roi();
  bool get_response_files();
  bool get_emulate_syscalls();
  Sift::Mode get_current_mode();
  uint32_t get_app_id();
  void set_app_id(uint32_t appId);
  std::string get_output_file();
  bool get_send_physical_address();
  uint64_t get_flow_control();
  uint64_t get_flow_control_ff();
  bool get_ssh();
  uint64_t get_stop_address();

  /// Set protected fields
  void set_current_mode(Sift::Mode mode);

  protected:
  /// Parsing of command line has been successful
  bool parsing_error;

  /// Current simulation mode
  Sift::Mode current_mode;

  /// Output information in verbose mode
  bool verbose;

  /// Simulation currently running in Region Of Interest
  bool use_roi;
  
  /// MPI threads use implicit ROI at finalizing
  bool mpi_implicit_roi;

  /// Number of instructions to fast forward the simulation
  uint64_t fast_forward_target;

  /// Number of instructions to trace in detail
  uint64_t detailed_target;

  /// Blocksize
  uint64_t blocksize;

  /// Output file for the traces
  std::string output_file;

  /// Syscall emulation on/off -- required for multithreaded applications
  bool emulate_syscalls;

  /// Use response files for communicating with Sniper -- required for multithreaded applications or for syscall
  /// emulation
  bool response_files;

  /// Send logical to physical address mapping on/off
  bool send_physical_address;

  /// Adress to stop the simulation
  uint64_t stop_address;

  /// ID of currently simulated application within frontend
  uint32_t app_id;
  
  /// Number of instructions to send before synchronizing with backend.
  uint64_t flow_control;  
  
  /// Number of instructions to batch up before sending instruction counts in fast-forward mode.
  uint64_t flow_control_ff;
  
  /// Frontend and backend communicate over the network
  bool ssh;
};

// TODO Change name from options to config or something similar
/**
 * @class FrontendOptions
 *
 * Generic interface for the frontend command line options.
 */

template <typename T> class FrontendOptions : public OptionsBase<T>
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

  /// Get private fields
  uint32_t get_ncores();
  std::string get_statefile();
  std::string get_cmd_app();
  FrontendISA get_theISA();

  private:
  /// To parse the command line arguments
  ez::ezOptionParser opt;

  /// Command line to launch the application to simulate
  std::string cmd_app;

  /// Number of cores -- not common to all frontends
  uint32_t ncores;

  /// Route to the file that keeps the state of the frontend for the next run -- not common to all frontends
  std::string statefile;
  
  /// ISA of the instructions sent to Sniper's backend
  FrontendISA theISA;
};

} // namespace frontend

#include "frontend_options-inl.h"
#include "frontend_options.tcc"

#endif // _FRONTEND_OPTIONS_H_
