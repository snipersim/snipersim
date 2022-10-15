#!/usr/bin/env python2
# coding: utf-8

import os, sys, getopt,copy
HOME = os.path.abspath(os.path.dirname(__file__))
sys.path.extend( [os.path.abspath(os.path.join(HOME, '..'))] )
import sniper_lib, sniper_config, sniper_stats
import functionparser, asomodules


#This module provides some functions that work with the asomodules.
#It also contains a main function that can give textual output.

#-----------------------------------------------------------------------#
#		Functions to run the modules			        #
#-----------------------------------------------------------------------#

#Run all the modules and return an optimizationlist
def runModules(functions,config):
  optimizationlist=[]
  for function in functions:
    optimizationlist.append(asomodules.ILPModule                                (function,config))
    optimizationlist.append(asomodules.TLPModule                                (function,config))
    optimizationlist.append(asomodules.PercentOfDataUtilizedModule              (function))
    optimizationlist.append(asomodules.BranchModule                             (function))
    optimizationlist.append(asomodules.NonFPModule                              (function))
    optimizationlist.append(asomodules.VectorizationModule                      (function))
    optimizationlist.append(asomodules.MemoryModule                             (function))
  return optimizationlist

#Combine and run all the modules and return an optimizationlist
def runCombinedModules(functions,config):
  combinationlist=[]
  for function in functions:
    #combinationlist.append(ILPModule(BranchModule(function),config))
    combinationlist.append(asomodules.ILPModule(
                             asomodules.TLPModule(
                               asomodules.PercentOfDataUtilizedModule(
                                 asomodules.BranchModule(
                                   asomodules.NonFPModule(
                                     asomodules.VectorizationModule(
                                       asomodules.MemoryModule(function)
                                     )
                                   )
                                 )
                               ) ,config)
                             ,config))
  return combinationlist


#-----------------------------------------------------------------------#
#               Functions to get info about the modules                 #
#-----------------------------------------------------------------------#


#Get the names of all optimization modules
def getOptimizationNames():
  return ['ILP','TLP','Branch','NonFP','Vectorization','Memory','PercentOfDataUtilized']

#Get optimization full names and summary
def getOptimizationSummary():
  summary = {}
  summary["ILP"]=(dict
	(name= "ILP", full_name= "Instruction Level Parallellism", 
	summary= "Use Instruction Level Parallellism to speed up the application."))
  summary["TLP"]=(dict
	(name= "TLP", full_name= "Thread Level Parallellism",
  	summary= "Use Thread Level Parallellism to speed up the application."))
  summary["Branch"]=(dict
	(name= "Branch", full_name= "Reduce branch instructions.",
	summary= "Reduce branch instructions to prevent costly branch predictor misses."))
  summary["NonFP"]=(dict
	(name= "NonFP", full_name= "Remove non-floating point instructions",
	summary= "Remove non-floating point instructions, this will get you closer to the roofline."))
  summary["Vectorization"]=(dict
	(name= "Vectorization", full_name= "Vectorization (Single Instruction Multiple Data instructions)",
	summary= "Use Vectorization (Single Instruction Multiple Data instructions) to execute more instructions per cycle."))
  summary["Memory"]=(dict
	(name= "Memory", full_name= "Reduce memory operations",
	summary= "Reduce costly memory operations to speed up the application."))
  summary["PercentOfDataUtilized"]=(dict
        (name= "PercentOfDataUtilized", full_name= "Reduce wasted DRAM bandwidth",
        summary= "Reduce wasted DRAM bandwidth. For example with AOS to SOA (Array Of Structures to Structures of Arrays)"))
  return summary


#-----------------------------------------------------------------------#
#               	Help functions				        #
#-----------------------------------------------------------------------#


#sort an optimizationlist by the time won back
def sortByTimeWonBack(optlist):
  #print optlist
  newlist = sorted(optlist, key=lambda k: k['time_won_back'])
  newlist.reverse()
  return newlist

#get a function by id
def getFunctionById(functions,idnr):
  for function in functions:
    if function["id"]==idnr:
      return function
  return 

#convert the optimizationlist to a list only containing the optimizations and their speedups
def getOptimizationOutput(optlist, totaltime, originalfunctions):
  prettylist=[]
  sortedlist=sortByTimeWonBack(optlist)
  for optimization in sortedlist:
    originalfunction 	= getFunctionById(originalfunctions, optimization["id"])
    originaltime	= originalfunction["nonidle_elapsed_time"]
    if len(optimization["optimizations"]) > 0:
      prettylist.append(dict(
        optimization	=optimization["optimizations"],
        time_won_back	=optimization["time_won_back"],
        time_won_back_pct	=optimization["time_won_back"]/originaltime*100,
        app_speedup	=totaltime/(totaltime-optimization["time_won_back"]),
        function		=optimization["name"],
        name_clean	=optimization["name_clean"],
        id		=optimization["id"]
    ))
  return prettylist


#-----------------------------------------------------------------------#
#                       Get ordered optimization lists                  #
#-----------------------------------------------------------------------#

#-----------------------Per function------------------------------------#

#get top optimizations for a given function
def getOptimizations(optimizationlist,functionid, totaltime, originalfunctions):
  optimizations = []
  for optimization in optimizationlist:
    if optimization["id"]==functionid:
      optimizations.append(optimization)
  prettylist = getOptimizationOutput(optimizations, totaltime, originalfunctions)
  return prettylist

#get optimizations per function
def getOptimizationsPerFunction(optimizationlist,functions, totaltime):
  optimizations = []
  for function in functions:
    optimizations.append(getOptimizations(optimizationlist,function["id"], totaltime, functions))
  return optimizations

#----------------------Overall------------------------------------------#

#get top n overall optimizations
def getTopOptimizations(optimizationlist,totaltime,n,originalfunctions):
  newlist = sortByTimeWonBack(optimizationlist)[0:min(n,len(optimizationlist))]
  prettylist  = getOptimizationOutput(newlist, totaltime, originalfunctions)
  return prettylist

#---------------------Per optimization----------------------------------#

#get top functions for a given optimization
def getFunctionsPerOptimization(optimizationlist,optimization, totaltime, originalfunctions):
  functionlist = []
  for function in optimizationlist:
    if len(function["optimizations"]) > 0 and function["optimizations"][0]["optimization"]==optimization:
      functionlist.append(function)
  prettylist = getOptimizationOutput(functionlist, totaltime, originalfunctions)
  return prettylist

#def get top n functions per optimization
def getTopOptimizationsPerModule(optimizationlist,totaltime,n, originalfunctions):
  optlist = []
  for opt in getOptimizationNames():
    optlistpermodule = getFunctionsPerOptimization(optimizationlist,opt,totaltime, originalfunctions)
    optlist.append(optlistpermodule[0:min(n,len(optlistpermodule))])
  return optlist

#-----------------------------------------------------------------------#
#               	Print functions				        #
#-----------------------------------------------------------------------#

#print an ordered list of optimizations
def prettyPrint(optimizationlist,totaltime,numberofresults,originalfunctions):
  newlist = sortByTimeWonBack(optimizationlist)
  i=0
  for function in newlist[0:min(numberofresults,len(optimizationlist))]:
    originalfunction    = getFunctionById(originalfunctions, function["id"])
    originaltime        = originalfunction["nonidle_elapsed_time"]

    print "#"+str(i+1),         function["name_clean"], "id: ",function["id"]
    print "Optimization: ",     function["optimizations"]
    print "Time won back: ",    function["time_won_back"], "("+str(function["time_won_back"]/originaltime*100)+"% of function time)"
    print "Speedup factor for application: ","x"+str(totaltime/(totaltime-function["time_won_back"]))
    print
    i+=1
    
#print overall result
def printResult(optimizationlist,totaltime,numberofresults, originalfunctions):
  print "Top "+str(numberofresults)+" optimizations (sorted by absolute time gain)"
  print "===================="
  prettyPrint(optimizationlist,totaltime,numberofresults, originalfunctions)


#-----------------------------------------------------------------------#
#                       Main function                                   #
#-----------------------------------------------------------------------#


#main function
if __name__ == '__main__':
  def usage():
    print('Usage: '+sys.argv[0]+' [-h|--help (help)] [-d <resultsdir (default: .)>] [-j <jobid>] [-n number of results (default: 10)] [-v|--verbose]')
    sys.exit()

  resultsdir = '.'
  outputdir = '.'
  verbose = False
  numberofresults = 10
  jobid = None

  totaltime=0

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hd:n:vj:", [ "help", "verbose" ])
  except getopt.GetoptError, e:
    print(e)
    usage()
    sys.exit()
  for o, a in opts:
    if o in ('-h', '--help'):
      usage()
    if o == '-d':
      resultsdir = a
    if o == '-n':
      numberofresults = int(a)
    if o == '-v' or o == '--verbose':
      verbose = True
    if o == '-j':
      jobid = int(a)


  if verbose:
    print 'This script generates automatic suggestions for optimization'
  
  config = sniper_lib.get_config(jobid = jobid, resultsdir = resultsdir)
  if verbose:
    print "parsing functions from sim.rtntrace"
  for fn in ("sim.rtntrace", "rtntrace.out"):
    rtntrace = sniper_lib.get_results_file(filename = fn, jobid = jobid, resultsdir = resultsdir)
    if rtntrace:
      break
  functions,total=functionparser.parseFunctions(inputdata = rtntrace)
  optimizationlist = runModules(functions,config)
  totaltime = total["nonidle_elapsed_time"]
  print
  print "Overall best optimizations"
  printResult(optimizationlist,totaltime,numberofresults,functions)

  print 
  print "Overall best combined optimizations"
  combinedlist = runCombinedModules(functions,config)
  printResult(combinedlist, totaltime, numberofresults,functions)
