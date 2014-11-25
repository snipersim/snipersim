#!/usr/bin/env python2
# coding: utf-8

import os, sys, getopt, re, math, subprocess,json,copy
HOME = os.path.abspath(os.path.dirname(__file__))
sys.path.extend( [os.path.abspath(os.path.join(HOME, '..'))] )
import sniper_lib, sniper_config, sniper_stats
import functionparser, aso, asohelper

#This script generates data for the function-based visualizations.
#These visualizations contain:
#				* roofline model
#				* instructions vs time plot
#				* automatic suggestions for optimization


#-----------------------------------------------------------------------#
#               Global variables	                                #
#-----------------------------------------------------------------------#

functiondata 		= []
functionpercentages 	= []
functionboundaries 	= {}

total = dict(
  calls 	= 0,
  icount 	= 0,
  core_time 	= 0,
  nonidle_time 	= 0,
  l3miss 	= 0
)

#-----------------------------------------------------------------------#
#               Retrieve information                                    #
#-----------------------------------------------------------------------#

#For each property, get the function with the minimum and the maximum of that property
def getMaxFunctions(fdata):
  functionboundaries={}
  for key in fdata[0]:
    seq = [x[key] for x in fdata]
    functionboundaries[key]=dict(
				min=seq.index(min(seq)), 
 				max=seq.index(max(seq)))
  return functionboundaries

#Read inputdata				
def readInputData(inputfile):
  global functiondata, total
  functiondata, total = functionparser.parseFunctions(inputfile)

#Run the ASO modules and get the optimization info
def getOptimizationInfo():
  global functiondata, config
  optimizationinfo={}
  optimizationlist 			= aso.runModules(functiondata,config)
  optimizationinfo["top_optimizations"] = aso.getTopOptimizations(optimizationlist, total["nonidle_elapsed_time"],10,functiondata)
  optimizationinfo["per_function"] 	= aso.getOptimizationsPerFunction(optimizationlist,functiondata,total["nonidle_elapsed_time"])
  optimizationinfo["per_module"]	= aso.getTopOptimizationsPerModule(optimizationlist,total["nonidle_elapsed_time"],10, functiondata)
  optimizationinfo["optimization_names"]= aso.getOptimizationNames()
  optimizationinfo["summary"]		= aso.getOptimizationSummary()
  combinedlist				= aso.runCombinedModules(functiondata,config)
  optimizationinfo["combined"]		= aso.getTopOptimizations(combinedlist, total["nonidle_elapsed_time"],10,functiondata)
  return optimizationinfo

#calculate Peak Floating Point Performance in GFlops/s
def getPeakFPPerformance():
  frequency     = float(config["perf_model/core/frequency"])
  cores         = float(config["general/total_cores"])
  ports         = 2 #one port for mul/div and one for add/sub
  peakfloatperf = float(frequency*cores*ports)
  return peakfloatperf

#calculate the peak memory bandwidth
def getPeakMemBandwidth():
  #per controller bandwidth (GB/s)
  per_controller_bandwidth      =config['perf_model/dram/per_controller_bandwidth']
  #number of controllers
  dramcntlrs                    = [ lid for (name, lid, mid) in stats.get_topology() if name == 'dram-cntlr' ]
  numdramcontrollers            = len(dramcntlrs)
  totalbandwidth                = per_controller_bandwidth*numdramcontrollers
  return totalbandwidth

#getTotalCPIoffunction
def getCPI(function):
  frequency 	= float(config["perf_model/core/frequency"])
  clockperiod	= (1/frequency)*1e6 #femtoseconds
  icount	= function["instruction_count"]
  time		= function["nonidle_elapsed_time"]
  cpi		= time/(clockperiod*icount) 
  return cpi  

#-----------------------------------------------------------------------#
#               Write JSON files                                        #
#-----------------------------------------------------------------------#

#write stats about time vs number of instructions
#difference between core elapsed time and non-idle time?
def writeiptstats(outputfile):
  output={}
  output["iptdata"]=[]
  for data in functiondata:
    output["iptdata"].append([data["nonidle_elapsed_time"]/1e6,[data["instruction_count"]]])
    functionpercentages.append(dict(
      calls			=float(data["calls"]/total["calls"]),
      icount			=float(data["instruction_count"]/total["instruction_count"]),
      nonidle_elapsed_time	=float(data["nonidle_elapsed_time"]/total["nonidle_elapsed_time"]),
      l3miss			=float(data["l3miss"]/total["l3miss"]),
    ))
  output["functionpercentages"]	=functionpercentages
  functioninfo = copy.deepcopy(functiondata)
  #convert femtoseconds to nanoseconds:
  for function in functioninfo:
    function["cpi"]=getCPI(function)
    function["nonidle_elapsed_time"]/=1e6


  output["functioninfo"]	=functioninfo
  output["functionboundaries"]	=getMaxFunctions(functioninfo)
  output["optimizationinfo"]	=getOptimizationInfo()
  f = open(outputfile, "w")
  f.write(json.dumps(output,indent=4))
  f.close()


#write JSON file with rooflinestats
def writerooflinestats(outputfile):
  output = {}
  output["rooflinedata"]=[]
  for data in functiondata:
    x=0
    y=0
    fpinstr = (asohelper.get_fp_addsub(data)+asohelper.get_fp_muldiv(data))
    if (data["l3miss"]) > 0:
      x = float((fpinstr/data["l3miss"])/64) #per byte, so division by 64
    if (data["nonidle_elapsed_time"]) > 0:
      y = float(fpinstr/data["nonidle_elapsed_time"]*1e6) #GFLOPS
    output["rooflinedata"].append([x,y])
  
  functioninfo = copy.deepcopy(functiondata)
  for function in functioninfo:
    function["nonidle_elapsed_time"]/=1e6
    function["cpi"]=getCPI(function)
  output["functioninfo"]=functioninfo
  output["functionpercentages"]=functionpercentages
  output["peakfpperformance"]=getPeakFPPerformance()
  output["peakmembandwidth"]=getPeakMemBandwidth()
  f = open(outputfile, "w")
  f.write(json.dumps(output,indent=4))
  f.close()

#-----------------------------------------------------------------------#
#               	Doxygen                                         #
#-----------------------------------------------------------------------#


#create DoxyGen output
def createDoxyGenOutput(source,doxygenpath,title):
  os.system('cp "%s" "%s" ' % ("levels/functionbased/Doxyfile-template","levels/functionbased/Doxyfile"))
  f = open("levels/functionbased/Doxyfile","a")
  f.write("PROJECT_NAME 	       = "+title+"\n")
  f.write("INPUT                  = "+source)
  f.close()
  os.system('"%s" "%s"; ' % (doxygenpath+"doxygen","levels/functionbased/Doxyfile"))


#-----------------------------------------------------------------------#
#                       External function                               #
#-----------------------------------------------------------------------#

def createJSONData(resultsdir, outputdir, title = None, source = None, doxygenpath = None):

  resultsdir = os.path.abspath(resultsdir)
  outputdir = os.path.abspath(outputdir)
  if not title:
    title = os.path.basename(resultsdir)
  title = title.replace(' ', '_')

  global config, stats
  config = sniper_config.parse_config(file(os.path.join(resultsdir, 'sim.cfg')).read())
  stats = sniper_stats.SniperStats(resultsdir)

  readInputData(os.path.join(resultsdir,"sim.rtntrace"))
  if not os.path.exists(os.path.join(outputdir,"levels","functionbased")):
    os.makedirs(os.path.join(outputdir,"levels","functionbased"))

  writeiptstats(os.path.join(outputdir,"levels","functionbased","iptstats.json"))
  writerooflinestats(os.path.join(outputdir,"levels","functionbased","rooflinestats.json"))

  if not os.path.exists(os.path.join(HOME,"levels","functionbased","doxygen")):
    os.makedirs(os.path.join(HOME,"levels","functionbased","doxygen"))

  if source and doxygenpath:
    createDoxyGenOutput(source,doxygenpath,title)


#-----------------------------------------------------------------------#
#                       Main function                                   #
#-----------------------------------------------------------------------#

if __name__ == '__main__':
  def usage():
    print
    print('Usage: '+sys.argv[0]+' [-h|--help (help)] [-d <resultsdir (default: .)>] [-o <outputdir (default: .)>] [-t <title>] [-v|--verbose] [-s <sourcecodepath>] [-x <doxygen bin path>]')
    print
    print "This script generates data for the function-based visualizations."
    print "These visualizations contain:"
    print "                     * roofline model"
    print "                     * instructions vs time plot"
    print "                     * automatic suggestions for optimization"

    sys.exit()

  resultsdir = '.'
  outputdir = '.'
  title = None
  verbose = False
  source = None
  doxygenpath = None


  try:
    opts, args = getopt.getopt(sys.argv[1:], "hd:o:t:v:s:x:", [ "help", "verbose" ])
  except getopt.GetoptError, e:
    print(e)
    usage()
    sys.exit()
  for o, a in opts:
    if o in ('-h', '--help'):
      usage()
    if o == '-d':
      resultsdir = a
    if o == '-o':
      outputdir = a
    if o == '-t':
      title = a
    if o == '-s':
      source = a
    if o == '-x':
      doxygenpath = a
    if o == '-v' or o == '--verbose':
      verbose = True


  if verbose:
    print 'This script generates data for the roofline model'

  createJSONData(resultsdir, outputdir, title, source, doxygenpath)

  #write static files
  if outputdir != HOME:
    print "Copy files to output directory "+outputdir
    os.system('cd "%s"; tar c flot/ levels/functionbased/functionbased.html levels/functionbased/*js css/ levels/functionbased/doxygen | tar x -C %s' % (HOME, outputdir))

