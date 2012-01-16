#!/usr/bin/env python

# Look at a configuration file that tells which all tests to run
# Right now, run those tests one by one

# Import other configuration parameters from carbon_sim.cfg that you want
# to change

import os
import re
import sys
import subprocess
import numpy

# TODO
# Add support for handling comments

pintool_fixed_param_list = []
pintool_variable_param_list = []
sim_flags_list = []
num_procs_list = []
sim_core_index_list = []
app_name_list = []
app_flags_list = []
test_args_list = []
user_thread_index_list = []

plot_quantities_list = []
plot_core_dict = {}
plot_directory_list = []
aggregate_stats = {}

def run_simulation(is_dryrun, run_id, sim_root, experiment_directory, carbon_sim_config_filename):
   global sim_flags_list
   global num_procs_list
   global sim_core_index_list
   global app_name_list
   global app_flags_list
   global user_thread_index_list

   j = 0
   while j < len(app_name_list):
      i = 0
      while i < len(sim_flags_list):
         if (user_thread_index_list[j] == -1) or (sim_core_index_list[i] == -1) or (sim_core_index_list[i] == user_thread_index_list[j]):
            # Copy the results into a per-experiment per-run directory
            run_directory = experiment_directory + "ARGS_" + remove_unwanted_symbols(sim_flags_list[i]) + app_name_list[j] + "_" + remove_unwanted_symbols(app_flags_list[j]) + "_" + str(run_id) + "/"
            
            carbon_sim_cfg = sim_root + "/" + carbon_sim_config_filename

            curr_sim_flags = "-c " + carbon_sim_cfg + " " + sim_flags_list[i] + " --general/output_dir=\\\"" + run_directory + "\\\""
            
            command = "time make -C " + sim_root + \
                  " " + app_name_list[j] + \
                  " PROCS=" + num_procs_list[i] + \
                  " CONFIG_FILE=" + carbon_sim_cfg + \
                  " SIM_FLAGS=\"" + curr_sim_flags + "\"" + \
                  " APP_FLAGS=\"" + app_flags_list[j] + "\"" + \
                  " >& " + run_directory + "stdout.txt"
            print command

            if is_dryrun == 0:
               mkdir_command = "mkdir " + run_directory
               os.system(mkdir_command)
            
               proc = subprocess.Popen(command, shell=True)
               proc.wait()

               make_exec_file_command = "echo \"" + command + "\" > " + run_directory + "exec_command.txt"
               os.system (make_exec_file_command)
         i = i+1
      j = j+1

   return

def remove_unwanted_symbols(in_string):
   
   i = 0
   out_string = ''
   while i < len(in_string):
      if in_string[i] == ' ':
         out_string += '___'
      elif in_string[i] == '-':
         pass
      elif in_string[i] == '/':
         out_string += '__'
      elif re.search(r'\w', in_string[i]):
         out_string += in_string[i]
      else:
         out_string += '_'
      i += 1

   return out_string

# Parsing config files
def parse_config_file_params(tests_config_filename):
   tests_config_app = []
   tests_config_pintool = []
   plot_config = []

   parsing_app_list = 0
   parsing_pintool_params = 0
   parsing_plot_params = 0

   try:
      tests_config = open(tests_config_filename,"r").readlines()
   except IOError:
      print "Cannot Open File: %s" % tests_config_filename
      sys.exit(-1)

   for line in tests_config:
      if re.search(r'^\[apps\]', line):
         parsing_app_list = 1
      elif re.search(r'^\[/apps\]', line):
         parsing_app_list = 0
      elif re.search(r'^\[pintool\]', line):
         parsing_pintool_params = 1
      elif re.search(r'^\[/pintool\]', line):
         parsing_pintool_params = 0
      elif re.search(r'^\[plot\]', line):
         parsing_plot_params = 1
      elif re.search(r'^\[/plot\]', line):
         parsing_plot_params = 0
      elif re.search(r'^\s*\#', line):
         pass # Dont parse comments
      elif parsing_app_list == 1:
         tests_config_app.append(line)
      elif parsing_pintool_params == 1:
         tests_config_pintool.append(line)
      elif parsing_plot_params == 1:
         plot_config.append(line)

   parse_app_list(tests_config_app)
   curr_num_procs = parse_pintool_params(tests_config_pintool)
   parse_plot_list(plot_config)
   
   return curr_num_procs

def parse_app_list(tests_config_app):
   global app_name_list
   global app_flags_list

   for line in tests_config_app:
      if (re.search(r'[^\s]', line)):
         test_match = re.search(r'^\s*([-\w]+)(.*)', line)
         assert test_match
         test_name = test_match.group(1)
         test_args = test_match.group(2)

         parse_test_args(test_args)

         for test_instance in test_args_list:
            app_name_list.append(test_name)
            app_flags_list.append(test_instance)
   return

def parse_test_args(test_args):
   global test_args_list
   global user_thread_index_list

   var_arg_list = re.findall(r'\[[^\]]+\]', test_args)
   
   expanded_var_arg_list = []
   num_threads_pos = -1
   iter = 0
   for var_arg in var_arg_list:
      [is_num_threads, expanded_var_arg] = expand_to_list(var_arg)
      if is_num_threads == 1:
         num_threads_pos = iter
      iter = iter + 1
      expanded_var_arg_list.append(expanded_var_arg)

   splitted_test_args = re.findall(r'[^\[\]]+', test_args)
   
   # Remove all the odd indices in splitted_test_args
   fixed_test_arg_list = []
   iter = 0
   for arg in splitted_test_args:
      if (iter % 2) == 0:
         fixed_test_arg_list.append(arg)
      iter = iter + 1

   if (len(fixed_test_arg_list) != (len(expanded_var_arg_list)+1)):
      assert len(fixed_test_arg_list) == len(expanded_var_arg_list)
      fixed_test_arg_list.append(" ")
  
   test_args_list = []
   generate_test_args(fixed_test_arg_list, expanded_var_arg_list, num_threads_pos)
      
   return

def generate_test_args(fixed_test_arg_list, expanded_var_arg_list, num_threads_pos, curr_num_threads_index = -1, curr_test_arg_list = "", index = 0):
   global test_args_list
   global user_thread_index_list

   if index == len(expanded_var_arg_list):
      test_args_list.append(curr_test_arg_list + fixed_test_arg_list[index])
      user_thread_index_list.append(curr_num_threads_index)
      return

   # Initialize curr_num_threads_index
   if index == num_threads_pos:
      curr_num_threads_index = 0

   for arg in expanded_var_arg_list[index]:
      generate_test_args(fixed_test_arg_list, expanded_var_arg_list, 
            num_threads_pos, curr_num_threads_index,
            curr_test_arg_list + fixed_test_arg_list[index] + arg, index + 1)
      
      if index == num_threads_pos:
         curr_num_threads_index = curr_num_threads_index + 1
   
   return

def expand_to_list(var_arg):
   value_list = re.findall(r'[+*\.\w]+', var_arg)

   is_num_threads = 0
   if re.search(r'\$', var_arg):
      is_num_threads = 1
   
   if (re.search(r',', var_arg)):
      return [is_num_threads, value_list]

   assert(re.search(r':', var_arg))

   act_value_list = []
   
   assert(len(value_list) == 3)
   start_val = int(value_list[0])
   end_val = int(value_list[1])
   curr_val = start_val
            
   if (value_list[2][0] == '+'):
      increment = int(value_list[2][1:])
      while (curr_val <= end_val):
         act_value_list.append(str(curr_val))
         curr_val += increment
   elif (value_list[2][0] == '*'):
      multiplier = int(value_list[2][1:])
      while (curr_val <= end_val):
         act_value_list.append(str(curr_val))
         curr_val *= multiplier
   else:
      # Raise an exception
      assert(false)

   return [is_num_threads, act_value_list]


def parse_pintool_params(tests_config_pintool):

   global pintool_fixed_param_list
   global pintool_variable_param_list

   param_value_list_regex = re.compile(r'(\w+)\s*=\s*(\[.+\])')
   param_value_regex = re.compile(r'(\w+)\s*=\s*(\w+)')
   section_name_regex = re.compile(r'\[(.*)\]')

   section_name = ""
   curr_num_procs = '0'
   
   for line in tests_config_pintool:
    
      param_value_list_match = param_value_list_regex.search(line)
      if param_value_list_match:
         param = param_value_list_match.group(1)
         act_value_list = expand_to_list(param_value_list_match.group(2))[1]

         pintool_param = [section_name + "/" + param, act_value_list]
         pintool_variable_param_list.append(pintool_param)
         continue

      param_value_match = param_value_regex.search(line)
      if param_value_match:
         param = param_value_match.group(1)
         value = param_value_match.group(2)

         pintool_param = [section_name + "/" + param, value]
         if pintool_param[0] == "general/num_processes":
            curr_num_procs = pintool_param[1]
         # Construct the parameter to be passed to the pintool
         pintool_fixed_param_list.append(pintool_param)
         continue
 
      section_name_match = section_name_regex.search(line)
      if section_name_match:
         section_name = section_name_match.group(1)
   
   return curr_num_procs

def generate_pintool_args(arg_list, curr_num_procs, curr_core_index = -1, variable_param_num = 0):
   global pintool_variable_param_list
   global pintool_fixed_param_list
   global sim_flags_list
   global num_procs_list
   global sim_core_index_list

   # Recursive calls are made to this function

   if variable_param_num == len(pintool_variable_param_list):
      sim_flags_list.append(arg_list)
      num_procs_list.append(curr_num_procs)
      sim_core_index_list.append(curr_core_index)
      return
   
   pintool_param = pintool_variable_param_list[variable_param_num]
   param_name = pintool_param[0]
   value_list = pintool_param[1]
      
   # Initialize 'curr_core_index'
   if (param_name == 'general/total_cores'):
      curr_core_index = 0

   for value in value_list:
      if (param_name == 'general/num_processes'):
         curr_num_procs = value

      new_arg_list = arg_list + "--" + param_name + "=" + value + " "
      
      generate_pintool_args(new_arg_list, curr_num_procs, curr_core_index, variable_param_num+1)
      
      if (param_name == 'general/total_cores'):
         curr_core_index = curr_core_index + 1

   return

def parse_fixed_param_list():
   global pintool_fixed_param_list
   fixed_arg_list = ""
   for pintool_param in pintool_fixed_param_list:
      fixed_arg_list += "--" + pintool_param[0] + "=" + pintool_param[1] + " "
   return fixed_arg_list

def parse_plot_list(plot_config):
   global plot_quantities_list
   global plot_core_dict

   for line in plot_config:
      if (re.search(r'[^\s]', line)):
         plot_match = re.search(r'^\s*(\w[^\[]+)\[(.*)\]', line)
         assert plot_match
         plot_quantity = plot_match.group(1).strip()
         plot_cores = plot_match.group(2).strip()

         plot_quantities_list.append(plot_quantity)
         plot_core_dict[plot_quantity] = [str.strip() for str in plot_cores.split(",")]

   return

def generate_plot_directory_list(experiment_directory, runs):
   global plot_directory_list
   global app_name_list
   global app_flags_list
   i = 0
   while i < len(sim_flags_list):
      j = 0
      while j < len(app_name_list):
         if (user_thread_index_list[j] == -1) or (sim_core_index_list[i] == -1) or (sim_core_index_list[i] == user_thread_index_list[j]):
            for run in runs:
               plot_directory = experiment_directory + "ARGS_" + remove_unwanted_symbols(sim_flags_list[i]) + app_name_list[j] + '_' + remove_unwanted_symbols(app_flags_list[j]) + "_" + str(run) + "/"
               plot_directory_list.append(plot_directory)
         j = j+1
      i = i+1

   return

def print_help_message():
   print "[Usage]:"
   print "\t./tools/run_tests.py [-dryrun] [-f tests_config_file] [-n number_of_runs]"
   print "[Defaults]:"
   print "\t tests_config_file -> tests.cfg"
   print "\t number_of_runs -> 1"
   print "\t dryrun -> OFF"
   sys.exit(1)

# Interface to access the plotting infrastructure

# Set plot parameters from a config file
def setPlotParams (plot_config_file, plot_data_directory, runs):
   curr_num_procs = parse_config_file_params(plot_config_file)
   generate_pintool_args(parse_fixed_param_list(), curr_num_procs)
   generate_plot_directory_list(plot_data_directory, runs)

# Plot directory list
def getPlotDirectoryList():
   """Return a copy of the plotting directory list"""
   return plot_directory_list[:]

def addPlotDirectories(directory_list):
   for directory in directory_list:
      if directory not in plot_directory_list:
         plot_directory_list.append(directory)
      else:
         print "**WARNING** '" + directory + "' already in directory list"

def clearPlotDirectoryList():
   del plot_directory_list[:]

# Plot quantities
def getPlotQuantitiesList():
   """Return a copy of the list of quantities to plot"""
   return plot_quantities_list[:]

def addPlotQuantity(quantity, core_list):
   if quantity in plot_quantities_list:
      print "**WARNING** Added quantity '" + quantity + "' already in plot list, updating corresponding core list"
      for core in core_list:
         if core not in plot_core_dict[quantity]:
            plot_core_dict[quantity].append(core)
      print "'" + quantity + "' data will now be collected for these cores:" + str(plot_core_dict[quantity])
   else:
      plot_quantities_list.append(quantity)
      plot_core_dict[quantity] = core_list

def getPlotCoreListForQuantity(quantity):
   if quantity in plot_quantities_list:
      return plot_core_dict[quantity][:]
   else:
      print "**WARNING** '" + quantity + "' not in plot quantities list"
      return []

def clearPlotQuantityList():
   for quantity in plot_quantities_list:
      del plot_core_dict[quantity]
      plot_quantities_list.remove(quantity)

# Getting results
def getPlotVal(quantity, directory, core):
   """ Returns the value corresponding to the 3-tuple (quantity, directory, core)
       The returned value is either a float or a list of floats (if the core is a 'Core *', 'TS *' etc)
   """
   return aggregate_stats[quantity][directory][core]

# Aggregating data
def aggregate_plot_data():
   global aggregate_stats
   global plot_quantities_list
   global plot_directory_list
   global plot_core_dict

   aggregate_stats = {}
   for quantity in plot_quantities_list:
      aggregate_stats[quantity] = {}

   for directory in plot_directory_list:
      stats_file = directory + "sim.out"
      try:
         stat_lines = open(stats_file, "r").readlines()
      except IOError:
         print("Cannot open file: %s" % stats_file)
         sys.exit(-1)
      
      stats = []
      for line in stat_lines:
         stats.append ([element.strip() for element in line.strip().split("|")])

      stats = numpy.array (stats)

      # Throw away the last column if it is empty
      if stats [0, stats.shape[1] - 1] == '':
         stats = stats[:, 0:(stats.shape[1] - 1)]

      for quantity in plot_quantities_list:
         try:
            match = numpy.where (stats[:, 0] == quantity)
            i = match[0][0]
         except IndexError:
            print "Could not find quantity '" + quantity + "' in file " + stats_file
            sys.exit(-1)
        
         aggregate_stats[quantity][directory] = {}
         for core in plot_core_dict[quantity]:
            mean_quantity_match = re.search(r'([\w\s]*)\*', core)
            if mean_quantity_match:
               core_match = mean_quantity_match.group(1).strip()
               if core_match == '':
                  val_list = numpy.array([float(stats[i][j]) for j in range(len(stats[0, :])) if re.search(r'[\w]+', stats[0][j])])
                  aggregate_stats[quantity][directory][core] = val_list
               else:
                  val_list = numpy.array([float(stats[i][j]) for j in range(len(stats[0, :])) if re.search(core_match, stats[0][j])])
                  aggregate_stats[quantity][directory][core] = val_list
            else:
               try:
                  match = numpy.where (stats[0, :] == core)
                  j = match [0][0]
               except IndexError:
                  print "Could not find core '" + core + "' in file " + stats_file
                  sys.exit(-1)
          
               aggregate_stats[quantity][directory][core] = float(stats [i][j])


