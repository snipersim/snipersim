#include "handle_args.h"

#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <stdarg.h>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>

static char*prog_name;

using namespace boost::algorithm;


void handle_generic_arg(const String &str, config::ConfigFile & cfg);
void handle_args(const string_vec & args);

void usage_error(const char*error_msg, ...)
{
   fprintf(stderr, "Error: ");
   va_list args;
   va_start(args, error_msg);
   vfprintf(stderr,  error_msg, args);
   va_end(args);
   fprintf(stderr, "\n");

   fprintf(stderr, "Usage: %s -c config [extra_options]\n", prog_name);
   exit(-1);
}

void parse_args(string_vec &args, String & config_path, int argc, char **argv)
{
   prog_name = argv[0];
   bool set_config_path = false;

   for(int i = 1; i < argc; i++)
   {
       if(strcmp(argv[i], "-c") == 0)
       {
           if(i + 1 >= argc)
               usage_error("Should have provided another argument to the -c parameter.\n");
           if(!set_config_path) {
               /* first -c or --config= option sets base configuration file, overriding default 'carbon_sim.cfg */
               config_path = argv[i+1];
               set_config_path = true;
           } else {
               /* subsequent -c or --config= option sets extra config file */
               args.push_back(String("--config=") + argv[i+1]);
           }
           i++;
       }
       else if(strncmp(argv[i],"--config", strlen("--config")) == 0)
       {
           if(!set_config_path)
           {
               /* first -c or --config= option */
               string_vec split_args;
               String config_arg(argv[i]);
               boost::split( split_args, config_arg, boost::algorithm::is_any_of("=") );

               if(split_args.size() != 2)
                   usage_error("Error parsing argument: %s (%s)\n", config_arg.c_str(), argv[i]);

               config_path = split_args[1];
               set_config_path = true;
           } else {
               /* subsequent -c or --config= option */
               args.push_back(argv[i]);
           }
       }
       else if(strcmp(argv[i], "--") == 0)
           return;
       else if(strncmp(argv[i], "--", strlen("--")) == 0)
       {
           args.push_back(argv[i]);
       }
   }

   if(config_path == "")
       usage_error("Should have specified config argument.\n");
}


void handle_args(const string_vec & args, config::ConfigFile & cfg)
{
    for(string_vec::const_iterator i = args.begin();
            i != args.end();
            i++)
    {
        handle_generic_arg(*i, cfg);
    }
}

void handle_generic_arg(const String &str, config::ConfigFile & cfg)
{
   string_vec split_args;

   boost::split( split_args, str, boost::algorithm::is_any_of("=") );

   if(split_args.size() != 2 || split_args[0].size() <= 2 ||
           split_args[0].c_str()[0] != '-' || split_args[0].c_str()[1] != '-')
       usage_error("Error parsing argument: %s\n", str.c_str());

   String setting(split_args[0].substr(2));
   String & value(split_args[1]);

   if (setting == "config")
   {
       /* Merge settings in new config file into the current ConfigFile object */
       cfg.load(value);
   }
   else
   {
      string_vec path;
      boost::split(path, setting, boost::algorithm::is_any_of("/"));

      // Build a valid configuration file from the command line parameters
      bool first = true;
      String built_config_file = "[";
      for (size_t i = 0 ; i < path.size()-1 ; i++ )
      {
         if (first)
         {
            first = false;
         }
         else
         {
            built_config_file += "/";
         }

         built_config_file += path[i];
      }
      built_config_file += "]\n" + path[path.size()-1] + "=" + value;

      cfg.loadConfigFromString(built_config_file);
   }
}
