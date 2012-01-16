#ifndef HANDLE_ARGS_H
#define HANDLE_ARGS_H

#include "config_file.hpp"
typedef std::vector< String > string_vec;

void parse_args(string_vec &args, String & config_path, int argc, char **argv);
void handle_args(const string_vec & args, config::ConfigFile & cfg);

#endif
