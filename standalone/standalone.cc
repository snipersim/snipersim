#include "simulator.h"
#include "handle_args.h"
#include "config.hpp"
#include "trace_manager.h"
#include "magic_client.h"
#include "logmem.h"
#include "exceptions.h"
#include "sim_api.h"

int main(int argc, char* argv[])
{
   // Set thread name for Sniper-in-Sniper simulations
   SimSetThreadName("main");

   // To make sure output shows up immediately, make stdout and stderr line buffered
   // (if we're writing into a pipe to run-graphite, or redirected to a file by the job runner, the default will be block buffered)
   setvbuf(stdout, NULL, _IOLBF, 0);
   setvbuf(stderr, NULL, _IOLBF, 0);

   const char *ld_orig = getenv("SNIPER_SCRIPT_LD_LIBRARY_PATH");
   if (ld_orig)
      setenv("LD_LIBRARY_PATH", ld_orig, 1);

   registerExceptionHandler();

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
