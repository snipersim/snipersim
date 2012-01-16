#!/usr/bin/env python

# Should always run this script from the carbon_sim top level directory

# Import all the required packages
import sys
import os
sys.path.append(os.getcwd() + '/tools')
import tests_infrastructure
import numpy

# Default values
plot_config_file = 'tests.cfg'
plot_data_directory = ''
runs = [0]

# Read values from the command line
expecting_file_name = 0
expecting_dir_name = 0

for argument in sys.argv:
   if expecting_file_name == 1:
      plot_config_file = argument
      expecting_file_name = 0
   elif expecting_dir_name == 1:
      plot_data_directory = argument
      expecting_dir_name = 0
   elif argument == '-f':
      expecting_file_name = 1
   elif argument == '-d':
      expecting_dir_name = 1
   elif argument == '-h':
      tests_infrastructure.print_plot_help_message()

# Set plotting params
tests_infrastructure.setPlotParams(plot_config_file, plot_data_directory, runs)

# Alternatively, one may do the following:
# 
# plot_directories = [....]
# plot_quantities = [...]
# plot_cores = {'quantity1': [], ...}
# tests_infrastructure.addPlotDirectories(plot_directories)
# for quantity in plot_quantities:
#  tests_infrastructure.addPlotQuantity(quantity, plot_cores[quantity])

# Aggregate plotting data
tests_infrastructure.aggregate_plot_data()

# Print the data
# Uncomment if needed
# for quantity in tests_infrastructure.getPlotQuantitiesList():
#    print quantity + ':' + '\n'
#    header = 'Directory' + '\t'
#    for core in tests_infrastructure.getPlotCoreListForQuantity(quantity):
#       header = header + core + '\t'
#    print header
#    for directory in tests_infrastructure.getPlotDirectoryList():
#       out = directory + '\t'
#       for core in tests_infrastructure.getPlotCoreListForQuantity(quantity):
#          out = out + str(tests_infrastructure.getPlotVal(quantity, directory, core)) + '\t'
#       print out
#    print '\n\n'

