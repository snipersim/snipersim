#include "simulator.h"
#include <cstdlib>
#include <sys/prctl.h>
#include <cstdio>
#include "handle_args.h"
#include "config.hpp"
#include "trace_manager.h"
#include "magic_client.h"
#include "logmem.h"
#include "exceptions.h"
#include "sim_api.h"

int main(int argc, char* argv[])
{
   /* SNIPER_ALLOW_PTRACE=1: let any process attach a debugger (gdb -p).
      Yama's default ptrace_scope refuses attaching to anything but a
      descendant, which makes a hung simulation impossible to inspect. */
   if (getenv("SNIPER_ALLOW_PTRACE"))
      prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
   // To make sure output shows up immediately, make stdout and stderr line
   // buffered (into run-sniper's pipe, or a file: the default is block buffered)
   setvbuf(stdout, NULL, _IOLBF, 0);
   setvbuf(stderr, NULL, _IOLBF, 0);
   // Set thread name for Sniper-in-Sniper simulations
   SimSetThreadName("main");

   const char *ld_orig = getenv("SNIPER_SCRIPT_LD_LIBRARY_PATH");
   if (ld_orig)
      setenv("LD_LIBRARY_PATH", ld_orig, 1);

   string_vec args;

   // Set the default config path if it isn't
   // overwritten on the command line.
   String config_path = "carbon_sim.cfg";

   parse_args(args, config_path, argc, argv);

   config::ConfigFile *cfg = new config::ConfigFile();
   cfg->load(config_path);

   handle_args(args, *cfg);

   Simulator::setConfig(cfg, Config::STANDALONE);

   Simulator::allocate();
   /* Before start(), not after: start() brings up the memory system,
      MimicOS, the scripts and, without --roi, the region of interest, and a
      crash in any of them ended the process with no backtrace at all.  The
      handler needs only the configuration's output directory. */
   registerExceptionHandler();
   Sim()->start();

   // config::Config shouldn't be called outside of init/fini
   // With Sim()->hideCfg(), we let Simulator know to complain when someone does call Sim()->getCfg()
   Sim()->hideCfg();


   LOG_ASSERT_ERROR(Sim()->getTraceManager(), "In standalone mode but there is no TraceManager!");
   Sim()->getTraceManager()->run();
   // Iterate over a number of application runs. This can allow for a warmup pass before running an application
   for (int i = 1 ; i < Sim()->getCfg()->getInt("traceinput/num_runs") ; i++)
   {
     Sim()->getTraceManager()->cleanup();
     Sim()->getTraceManager()->setupTraceFiles(i);
     Sim()->getTraceManager()->init();
     Sim()->getTraceManager()->run();
   }

   Simulator::release();
   delete cfg;

   return 0;
}
