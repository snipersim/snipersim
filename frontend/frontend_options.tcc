#include <string>

namespace frontend
{
  
template <typename T>
FrontendOptions<T>::FrontendOptions(int argc, const char * argv [])
{
  this-> parsing_error = true;
  this->current_mode = Sift::ModeIcount;
  this->verbose = false;
  this->ncores = 1;
  
  // TODO add to -frontend a name, with something like typeid(T).name(), or the solution described in http://stackoverflow.com/questions/1055452/c-get-name-of-type-in-template
  this->opt.syntax = "-frontend -ncores N -verbose [0,1] -roi [0,1] -f [#FF_instructions] -d [0,1] -b [blocksize] -o [outputfile] -e [0,1] -s [siftcountoffset] -r [0,1] -pa [0,1] -stop [stop_address] -statefile [route/file] -app [...]\n\n";
  this->opt.example = "-frontend -roi 1 -f 0 -d 0 -b 32 -o file.out -e 1 -s 0 -r 1 -pa 0 -stop 1000000 -app /bin/ls\n\n";
  this->opt.overview = "Frontend options";

  // Add command line options that can be processed
  this->opt.add(
    "0", // Default.
    0, // Required?
    1, // Number of args expected.
	0, // Delimiter if expecting multiple args.
	"Use ROI markers.", // Help description.
	"-roi"     // Flag token. 
  );
  this->opt.add("0", 0, 1, 0, "Verbose output.", "-verbose");
  this->opt.add("0", 0, 1, 0, "Number of frontend emulated cores.", "-ncores");
  this->opt.add("0", 0, 1, 0, "Number of instructions to fast forward.", "-f");
  this->opt.add("0", 0, 1, 0, "Number of instructions to trace in detail (default = all).", "-d");
  this->opt.add("0", 0, 1, 0, "Blocksize.", "-b");  
  this->opt.add("trace", 0, 1, 0, "Output file.", "-o");
  this->opt.add("", 0, 1, 0, "State file.", "-statefile");
  this->opt.add("0", 0, 1, 0, "Emulate syscalls (required for multithreaded applications, default = 0).", "-e");
  this->opt.add("0", 0, 1, 0, "SIFT app id (default = 0).", "-s");
  this->opt.add("0", 0, 1, 0, "Use response files (required for multithreaded applications or when emulating syscalls, default = 0).", "-r");
  this->opt.add("0", 0, 1, 0, "Send logical to physical address mapping.", "-pa");
  this->opt.add("0", 0, 1, 0, "Stop address (0 = disabled).", "-stop");
  this->opt.add("", 0, 1, 0, "Frontend state file.", "-o");
  this->opt.add("", 1, -1, ' ', "Application to simulate.", "-app");
  this->opt.add("x86_64", 1, -1, ' ', "ISA of the simulation.", "-isa");
 
  // Parse current command line
  this->opt.parse(argc, const_cast<const char**>(argv));

  // Save values of command line arguments 
  if (opt.isSet("-roi")) {
    int use_roi_value;
    this->opt.get("-roi")->getInt(use_roi_value);
    this->use_roi = use_roi_value != 0;  // bool conversion
  } else {
    this->use_roi = false;
  }

  if (opt.isSet("-verbose")) {
    int verbose_value;
    this->opt.get("-verbose")->getInt(verbose_value);
    this->verbose = verbose_value != 0;
  } else {
    this->verbose = false;
  }
  
  if (opt.isSet("-ncores")) {
    int ncores_value;
    this->opt.get("-ncores")->getInt(ncores_value);
    this->ncores = ncores_value;
  } else {
    this->ncores = 1;
  }
  
  if (opt.isSet("-f")) {
    int ff_value;
    this->opt.get("-f")->getInt(ff_value);
    this->fast_forward_target = ff_value;
  } else {
    this->fast_forward_target = 0;
  }
  
  if (opt.isSet("-d")) {
    int detailed_value;
    this->opt.get("-d")->getInt(detailed_value);
    this->detailed_target = detailed_value;
  } else {
    this->detailed_target = 0;
  }
  
  if (opt.isSet("-b")) {
    int blocksize_value;
    this->opt.get("-b")->getInt(blocksize_value);
    this->blocksize = blocksize_value;
  } else {
    this->blocksize = 0;
  }
    
  if (opt.isSet("-o")) {
    std::string outfile;
	this->opt.get("-o")->getString(outfile);
	this->output_file = outfile;
  } else {
    this->output_file = "trace";
  }
  
  if (opt.isSet("-statefile")) {
    std::string sfile;
	this->opt.get("-statefile")->getString(sfile);
	this->statefile = sfile;
  }
  
  if (opt.isSet("-e")) {
    int syscalls_value;
    this->opt.get("-e")->getInt(syscalls_value);
    this->emulate_syscalls = syscalls_value != 0;
  } else {
    this->emulate_syscalls = false;
  }
  
  if (opt.isSet("-s")) {
    int sift_value;
    this->opt.get("-s")->getInt(sift_value);
    this->app_id = sift_value;
  } else {
    this->app_id = 0;
  }
  
  if (opt.isSet("-r")) {
    int responsef_value;
    this->opt.get("-r")->getInt(responsef_value);
    this->response_files = responsef_value != 0;
  } else {
    this->response_files = false;
  }
 
  if (opt.isSet("-pa")) {
    int sendpa_value;
    this->opt.get("-pa")->getInt(sendpa_value);
    this->send_physical_address = sendpa_value != 0;
  } else {
    this->send_physical_address = false;
  }
  
  if (opt.isSet("-stop")) {
    int stop_value;
    this->opt.get("-stop")->getInt(stop_value);
    this->stop_address = stop_value;
  } else {
    this->stop_address = 0;
  }
  
  if (opt.isSet("-app")) {
    std::string cmd_value;
	this->opt.get("-app")->getString(cmd_value);
	this->cmd_app = cmd_value;
  }
  
  if (opt.isSet("-isa")) {
    std::string isaopt;
	this->opt.get("-isa")->getString(isaopt);
    if(isaopt == "arm32")
      this->theISA = ARM_AARCH32;
    else if(isaopt == "arm64")
      this->theISA = ARM_AARCH64;
    else if(isaopt == "ia32")
      this->theISA = INTEL_IA32;
    else if(isaopt == "x86_64")
      this->theISA = INTEL_X86_64;
    else
      this->theISA = UNDEF_ISA;
  } else {
    this->theISA = INTEL_X86_64;
  }
  
  
  this->parsing_error = false;

}

} // namespace frontend
