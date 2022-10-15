#!/usr/bin/env python2
# coding: utf-8

import os, sys, getopt, re, math, subprocess
HOME = os.path.abspath(os.path.dirname(__file__))
sys.path.extend( [os.path.abspath(os.path.join(HOME, '..'))] )
import sniper_lib, sniper_config, sniper_stats, cpistack, cpistack_items, mcpat, json


# From http://stackoverflow.com/questions/600268/mkdir-p-functionality-in-python
def mkdir_p(path):
  import errno
  try:
    os.makedirs(path)
  except OSError, exc:
    if exc.errno == errno.EEXIST and os.path.isdir(path):
      pass
    else: raise


def initialize():
  #get the component names from cpistack.py
  global cpiitems, cpiitemssimple
  cpiitems = cpistack_items.CpiItems(use_simple_mem = True)
  cpiitemssimple = cpistack_items.CpiItems(use_simple = True, use_simple_mem = True)

  #this list keeps the instruction count per interval, indexed by interval number
  global instructioncountlist
  instructioncountlist = []
  #this list keeps the partial sum of the instruction count, indexed by interval number
  global instructioncountsumlist
  instructioncountsumlist = []

  global cpicomponents, simplifiedcpicomponents, mcpatcomponents, cpificcomponents, simplifiedcpificcomponents
  cpicomponents = {}
  simplifiedcpicomponents = {}
  mcpatcomponents = {}
  cpificcomponents = {}
  simplifiedcpificcomponents = {}
  #list of available components
  global listofmcpatcomponents
  listofmcpatcomponents = mcpat.get_all_names()
  listofmcpatcomponents.append('other')

  #list of used components
  global usedcpicomponents, usedsimplifiedcpicomponents
  global usedmcpatcomponents, usedcpificcomponents
  usedcpicomponents = []
  usedsimplifiedcpicomponents = []
  usedmcpatcomponents = []
  usedcpificcomponents = [] #cpific == cpi with fixed instruction counts

  #list of ipcvalues
  global ipcvalues
  ipcvalues = [1]
  ipcvalues[0] = {}
  ipcvalues[0]["name"]="IPC"
  ipcvalues[0]["data"]=[0 for x in xrange(num_intervals)]

  #ipcvalues calculated with fixed instruction count intervals
  global ipcvaluesfic
  ipcvaluesfic = [1]
  ipcvaluesfic[0] = {}
  ipcvaluesfic[0]["name"]="IPC"


  for component in cpiitems.names:
    cpificcomponents[component]=[]
  for component in cpiitemssimple.names:
    simplifiedcpificcomponents[component]=[]
  #initialize data structures where the collected data will be stored
        #first column = x values
        #second column = cpipercentagevalues
        #third column = cpivalues
  for component in cpiitems.names:
    cpicomponents[component] = [[0 for x in xrange(3)] for x in xrange(num_intervals)]
        #first column = x values
        #second column = cpipercentagevalues
        #third column = cpivalues
  for component in cpiitemssimple.names:
    simplifiedcpicomponents[component] = [[0 for x in xrange(3)] for x in xrange(num_intervals)]
        #first column = x values
        #second column = power values
        #third column = energy values
        #fourth column = energypercentage values
  for component in listofmcpatcomponents:
    mcpatcomponents[component] = [[0 for x in xrange(4)] for x in xrange(num_intervals)]


#collect CPI stack data with fixed instruction counts
def collectCPIStackDataFIC(verbose=False, requested_cores_list = []):
  totalinstructioncount = 0
  groupedintervals = groupIntervalsOnInstructionCount(getTotalInstructionCount()/num_intervals, verbose)
  usedcomponents = dict.fromkeys(cpiitems.names,0)
  usedsimplecomponents = []
  ipcvaluesfic[0]["data"]=[dict(x=0,y=0) for x in xrange(len(groupedintervals))]
  for key in cpificcomponents.keys():
    cpificcomponents[key] = [[0 for x in xrange(2)] for x in xrange(len(groupedintervals))]
  for key in simplifiedcpificcomponents.keys():
    simplifiedcpificcomponents[key] = [[0 for x in xrange(2)] for x in xrange(len(groupedintervals))]
  for i in range (1, len(groupedintervals)):
    if verbose:
      print 'Collect CPI stack info for intervals with a fixed instruction count (interval '+str(i+1)+' / '+str(len(groupedintervals))+')'+"\r",
    cyclecountstart = groupedintervals[i-1]["cyclecount"]
    instructioncount = groupedintervals[i]["instructioncount"]
    nameintervalstart = groupedintervals[i-1]["intervalname"]
    nameintervalstop = groupedintervals[i]["intervalname"]
    currentinterval = (nameintervalstart, nameintervalstop)

    num_exceptions = 0
    simple=False

    try:
      results = cpistack.cpistack_compute(
        config = config,
        stats = stats,
        partial = currentinterval,
        use_simple = simple,
        use_simple_mem = True,
        no_collapse = True,
        aggregate = True,
        cores_list = requested_cores_list,
      )
      data = results.get_data('cpi')

      totalcpi=sum(data[0].itervalues())
      if totalcpi > 0:
        ipc = 1./totalcpi
      else:
        ipc = 0
      ipcvaluesfic[0]["data"][i]=dict(x=cyclecountstart/1e9, y=ipc)


      for key in results.labels:
        cpi = data[0][key]
        ipc = 0
        if cpi > 0.0:
          usedcomponents[key]=1
        cpificcomponents[key][i][0]=cpi
        cpificcomponents[key][i][1]=cyclecountstart/1e9 #now in microseconds
        simplecomponent = cpiitems.names_to_contributions[key]
        simplifiedcpificcomponents[simplecomponent][i][0]+=cpi
        simplifiedcpificcomponents[simplecomponent][i][1]=cyclecountstart/1e9 #now in microseconds
        if not simplecomponent in usedsimplecomponents:
          usedsimplecomponents.append(simplecomponent)

      totalinstructioncount+=instructioncount

    except ValueError:
      num_exceptions += 1
      totalinstructioncount+=instructioncount
      continue

  for component in cpiitems.names:
    if usedcomponents[component]==1:
      usedcpificcomponents.append(component)


  def writeJSON(components, usedcomponents, name):
    jsonoutput = [0 for x in xrange(len(usedcomponents))]
    index=0
    for key in usedcomponents:
      jsonoutput[index]={}
      jsonoutput[index]["name"]=key
      jsonoutput[index]["data"]=[0 for x in xrange(len(groupedintervals))]
      for i in range(0,len(groupedintervals)):
        xvalue = str(components[key][i][1])
        yvalue = str(components[key][i][0])
        jsonoutput[index]["data"][i]=dict(x=xvalue, y=yvalue)
      index+=1
    output = re.sub(r'("[xy]": )"([^\"]*)"',r'\1\2',json.dumps(jsonoutput, indent=4))
    mkdir_p(os.path.join(outputdir,'levels','level2','data'))
    jsonfile = open(os.path.join(outputdir,'levels','level2','data',title+'-'+name+'.json'), "w")
    jsonfile.write(output)
    jsonfile.close()

  writeJSON(cpificcomponents,usedcpificcomponents,'cpific')
  writeJSON(simplifiedcpificcomponents,usedsimplecomponents,'cpificsimple')

  if verbose:
    print


#Collect data with fixed cycle counts for the intervals
def collectCPIStackDataFCC(verbose = False, requested_cores_list = []):
  from StringIO import StringIO
  instructioncount=0
  num_exceptions=0
  usedcomponents = dict.fromkeys(cpiitems.names,0)

  for i in range(0,num_intervals):
    if verbose:
      print 'Collect CPI stack info for intervals with a fixed time span (interval '+str(i+1)+' / '+str(num_intervals)+')'+"\r",
    currentinterval = ("periodic-"+str(i*interval)+":periodic-"+str((i+1)*interval)).split(":")

    newinstructioncount=getInstructionCount(currentinterval)
    instructioncountlist.append(newinstructioncount)
    instructioncount+=newinstructioncount
    instructioncountsumlist.append(instructioncount)

    try:
      results = cpistack.cpistack_compute(
        config = config,
        stats = stats,
        partial = currentinterval,
        use_simple = False,
        use_simple_mem = True,
        no_collapse = True,
        aggregate = True,
        cores_list = requested_cores_list,
      )
      data = results.get_data('cpi')

      totalcpi=sum(data[0].itervalues())
      if totalcpi > 0:
        ipc = 1./totalcpi
      else:
        ipc = 0

      ipcvalues[0]["data"][i]=dict(x=i*interval/1e9, y=ipc)

      for key in results.labels:
        cpi = data[0][key]
        if totalcpi > 0:
          cpipercentage = 100.*cpi/totalcpi
        else:
          cpipercentage = 0

        if cpi > 0:
          usedcomponents[key]=1
        cpicomponents[key][i][0]=i
        cpicomponents[key][i][1]=cpipercentage
        cpicomponents[key][i][2]=cpi
        simplecomponent = cpiitems.names_to_contributions[key]
        simplifiedcpicomponents[simplecomponent][i][0]=i
        simplifiedcpicomponents[simplecomponent][i][1]+=cpipercentage
        simplifiedcpicomponents[simplecomponent][i][2]+=cpi
        if not simplecomponent in usedsimplifiedcpicomponents:
          usedsimplifiedcpicomponents.append(simplecomponent)

    except ValueError:
      ipcvalues[0]["data"][i]=dict(x=i, y=0)
      num_exceptions += 1
      continue

  for component in cpiitems.names:
    if usedcomponents[component]==1:
      usedcpicomponents.append(component)

  if verbose:
    print
    if(num_exceptions>0):
      print "There was no useful information for "+str(num_exceptions)+" intervals."
      print "You might want to increase the interval size."


def collectMcPATData(verbose = False):
  #Collecting data for McPat Visualization
  #print('Collecting data for mcpat visualization')
  from StringIO import StringIO
  for i in range(0,num_intervals):
    if verbose:
      print 'Collect McPAT info (interval '+str(i+1)+' / '+str(num_intervals)+')'

    data_to_return = mcpat.main(
      jobid = 0,
      resultsdir = resultsdir,
      partial = ["periodic-"+str(i*interval),"periodic-"+str((i+1)*interval)],
      powertype = 'dynamic',
      outputfile = 'power',
      no_graph = True,
      print_stack = False,
      return_data = True
    )

    components = data_to_return["labels"]
    powerdata = data_to_return["power_data"][0]
    time_s = data_to_return["time_s"]
    total = sum(data_to_return["power_data"][0].itervalues())

    for component in components:
      power = powerdata[component]/time_s
      energy = powerdata[component]
      energypercentage = 100*powerdata[component]/total
      if not component in usedmcpatcomponents:
        usedmcpatcomponents.append(component)
      mcpatcomponents[component][i][0]=i
      mcpatcomponents[component][i][1]=power
      mcpatcomponents[component][i][2]=energy
      mcpatcomponents[component][i][3]=energypercentage

  if verbose:
    print


#write values into json
#componentname = name of the component, e.g. power, energy, energypercentage, cpi...
#componenttype = type of the component, e.g. mcpat, cpi or cpisimplified
#componentindex = index of the y value
def writetojson(outputdir, componentname, componenttype, componentindex, verbose = False):
  if verbose:
    print 'Writing '+title+'-'+componentname+'.json'
  index=0
  if(componenttype == "cpi"):
    usedcomponents = usedcpicomponents
    components = cpicomponents
  elif(componenttype == "cpisimplified"):
    usedcomponents = usedsimplifiedcpicomponents
    components = simplifiedcpicomponents
  elif(componenttype == "mcpat"):
    usedcomponents = usedmcpatcomponents
    components = mcpatcomponents

  jsonoutput = [0 for x in xrange(len(usedcomponents))]

  for key in usedcomponents:
    jsonoutput[index]={}
    jsonoutput[index]["name"]=key
    jsonoutput[index]["data"]=[0 for x in xrange(num_intervals)]
    for i in range(0,num_intervals):
      if componenttype != "mcpat":
        xvalue = str(components[key][i][0]*interval/1e9) #x-axis now in microseconds
      else:
        xvalue = str(i*interval/1e9)
      yvalue = str(components[key][i][componentindex])
      jsonoutput[index]["data"][i]=dict(x=xvalue, y=yvalue)
    index+=1
  output = re.sub(r'("[xy]": )"([^\"]*)"',r'\1\2',json.dumps(jsonoutput, indent=4))
  mkdir_p(os.path.join(outputdir,'levels','level2','data'))
  jsonfile = open(os.path.join(outputdir,'levels','level2','data',title+'-'+componentname+'.json'), "w")
  jsonfile.write(output)
  jsonfile.close()

#This function calculates around which instruction we are at a given time
def calculateMarkerPosition(time):
  intervalindex=0
  while (interval*intervalindex / 1000000) < time and intervalindex < num_intervals:
    intervalindex+=1
  return instructioncountsumlist[intervalindex-1]



#write markers
def writemarkers(outputdir, verbose = False):
  if verbose:
    print 'Writing markers.txt'

  try:
    markersdb = stats.get_markers()
  except Exception, e:
    print e
    print 'Cannot get markers from database.'
    return

  markersjson = {}
  markersjson["markers"]=[]

  for timestamp, core, thread, value0, value1, description in markersdb:
    if description:
      marker = description
    else:
      marker = 'a = %d, b = %d' % (value0, value1)
    marker = 'T=%ld µs: %s' % (timestamp / 1e9, str(marker))
    markersjson["markers"].append(dict(timestamp=timestamp / 1e9, marker=marker))

  if verbose:
    print 'Found %d markers, writing markers.txt' % len(markersjson["markers"])

  mkdir_p(os.path.join(outputdir,'levels','level2','data'))
  markerstxt = open(os.path.join(outputdir,'levels','level2','data','markers.txt'), "w")
  markerstxt.write("markerstr = '"+json.dumps(markersjson)+"';\n")
  markerstxt.close()

# Write general info about the visualization in info.txt
def writeinfo(outputdir, verbose = False):
  if verbose:
    print 'Writing info.txt'
  mkdir_p(os.path.join(outputdir,'levels','level2','data'))
  info = open(os.path.join(outputdir,'levels','level2','data','info.txt'), "w")
  #info.write("infostr ='{ \"name\":\""+title+"\", \"intervalsize\":\""+str(interval)+"\", \"num_intervals\":\""+str(num_intervals)+"\",\"use_mcpat\":\""+str(use_mcpat)+"}';\n")
  info.write("infostr = '"+json.dumps(dict(name=title,intervalsize=interval,num_intervals=num_intervals,use_mcpat=use_mcpat))+"';\n")
  info.close()

# Write used lables in the info.txt file
def writelabels(outputdir, componentname, componenttype):
  mkdir_p(os.path.join(outputdir,'levels','level2','data'))
  labels = open(os.path.join(outputdir,'levels','level2','data','info.txt'), "a")
  labels.write("palette = new Rickshaw.Color.Palette( { scheme: 'munin' } );\n")
  if(componenttype == "cpi"):
    usedcomponents = usedcpicomponents
    ntc = cpiitems
  elif(componenttype == "cpisimplified"):
    usedcomponents = usedsimplifiedcpicomponents
    ntc = cpiitemssimple
  elif(componenttype == "mcpat"):
    usedcomponents = usedmcpatcomponents
  elif(componenttype == "cpific"):
    usedcomponents = usedcpificcomponents
    ntc = cpiitems

  jsonoutput = []
  if not componenttype == "mcpat":
    colors = ntc.get_colors(usedcomponents)
  output="''"
  for key in usedcomponents:
    if componenttype == "mcpat":
      jsonoutput.append(dict(name=key, color="palette.color()"))
      jsondump = json.dumps(jsonoutput)
      output = json.dumps(jsonoutput).replace("\"palette.color()\"",'palette.color()')
    else:
      jsonoutput.append(dict(name=key, color="rgb(%d,%d,%d)" % colors[key]))
      output = json.dumps(jsonoutput)

  labels.write(componentname+"labels = "+output+";\n")
  labels.close()


#write ipc values into json
def writeIPCvaluestoJSON(outputdir, verbose = False):
  if verbose:
    print 'Writing '+title+'-ipc.json'
  mkdir_p(os.path.join(outputdir,'levels','level2','data'))
  ipcjsonfile = open(os.path.join(outputdir,'levels','level2','data',title+'-ipc.json'), "w")
  ipcjsonfile.write(json.dumps(ipcvalues, indent=4))
  ipcjsonfile.close()
  ipcjsonfile = open(os.path.join(outputdir,'levels','level2','data',title+'-ipcfic.json'), "w")
  ipcjsonfile.write(json.dumps(ipcvaluesfic, indent=4))
  ipcjsonfile.close()


#return the total number of instructions processed in an interval
def getInstructionCount(intervalstr):
  results = sniper_lib.get_results(config = config, stats = stats, partial = intervalstr, metrics = ("performance_model.instruction_count",))
  instructioncount = sum(results["results"]["performance_model.instruction_count"])
  return instructioncount

def getTotalInstructionCount():
  results = sniper_lib.get_results(config = config, stats = stats, metrics = ("performance_model.instruction_count",))
  instructioncount = sum(results["results"]["performance_model.instruction_count"])
  return instructioncount


#groups intervals on a fixed instructioncount
def groupIntervalsOnInstructionCount(fixedinstructioncount, verbose=False):
  instructioncount = 0
  currentintervalnr=0
  if interval > 10 * native_interval:
    # When there are way more intervals than we'll use, don't look at all of them
    ratio = max(1, interval / native_interval / 10)
    interval_to_use = native_interval * ratio
    num_intervals_to_use = nativenum_intervals / ratio
  else:
    interval_to_use = native_interval
    num_intervals_to_use = nativenum_intervals
  currentintervalstr = ("periodic-"+str(currentintervalnr*interval_to_use), "periodic-"+str((currentintervalnr+1)*interval_to_use))
  intervalsequences = []
  intervalsequences.append(dict(cyclecount=0, instructioncount=0, intervalname=currentintervalstr[0]))
  nrofintervals = 0
  while currentintervalnr < num_intervals_to_use:
    if verbose:
      print "Put fixed time interval", currentintervalnr+1, "/", num_intervals_to_use, "in a fixed instruction count interval\r",
    instructioncount+=getInstructionCount(currentintervalstr)
    nrofintervals+=1
    if instructioncount > fixedinstructioncount:
      intervalsequences.append(dict(cyclecount=currentintervalnr*interval_to_use, instructioncount=instructioncount, intervalname=currentintervalstr[1]))
      #if nrofintervals < 4 :
      #  print "WARNING: less than 4 time-intervals in 1 instruction-interval"
      instructioncount=0
      nrofintervals=0
    currentintervalnr+=1
    currentintervalstr = ("periodic-"+str(currentintervalnr*interval_to_use), "periodic-"+str((currentintervalnr+1)*interval_to_use))

  if verbose:
    print
  return intervalsequences


def createJSONData(native_interval_, nativenum_intervals_, interval_, num_intervals_, resultsdir_, outputdir_, title_, mcpat, verbose = False, requested_cores_list = []):

  if verbose:
    print 'Generate JSON data for Level 2'

  global native_interval, nativenum_intervals, interval, num_intervals, resultsdir, outputdir, title, use_mcpat, stats, config
  native_interval = native_interval_
  nativenum_intervals = nativenum_intervals_
  interval = interval_
  num_intervals = num_intervals_
  resultsdir = resultsdir_
  outputdir = outputdir_
  title = title_
  use_mcpat = mcpat
  stats = sniper_stats.SniperStats(resultsdir_)
  config = sniper_lib.get_config(resultsdir = resultsdir_)

  initialize()

  collectCPIStackDataFIC(verbose = verbose, requested_cores_list = requested_cores_list)
  collectCPIStackDataFCC(verbose = verbose, requested_cores_list = requested_cores_list)

  writetojson(outputdir,"cpipercentage","cpi",1,verbose=verbose)
  writetojson(outputdir,"cpipercentagesimplified","cpisimplified",1,verbose=verbose)

  writeinfo(outputdir,verbose)
  writemarkers(outputdir,verbose)

  writelabels(outputdir,"cpipercentage","cpi")
  writelabels(outputdir,"cpipercentagesimplified","cpisimplified")
  writelabels(outputdir, "cpific","cpific")
  writelabels(outputdir, "simple","cpisimplified")

  writeIPCvaluestoJSON(outputdir)

  if(use_mcpat):
    collectMcPATData(verbose)
    writetojson(outputdir,"power","mcpat",1,verbose)
    writetojson(outputdir,"energy","mcpat",2,verbose)
    writetojson(outputdir,"energypercentage","mcpat",3,verbose)
    writelabels(outputdir,"power","mcpat")
    writelabels(outputdir,"energy","mcpat")
    writelabels(outputdir,"energypercentage","mcpat")

if __name__ == '__main__':
  def usage():
    print('Usage: '+sys.argv[0]+' [-h|--help (help)] [-d <resultsdir (default: .)>] [-o <outputdir (default: .)>] [-t <title>] [-n <num-intervals (default: 1000, all: 0)] [-i <interval (default: smallest_interval)> ] [--mcpat] [-v|--verbose] [-N <colon-separated-core-list>]')
    sys.exit()

  resultsdir = '.'
  outputdir = '.'
  title = None
  use_mcpat = False
  num_intervals = 1000
  interval = 0
  verbose = False
  requested_cores_list = []


  try:
    opts, args = getopt.getopt(sys.argv[1:], "hd:o:t:n:i:vN:", [ "help", "mcpat", "verbose" ])
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
    if o == '--mcpat':
      use_mcpat = True
    if o == '-n':
      num_intervals = long(a)
    if o == '-i':
      interval = long(a)
    if o == '-v' or o == '--verbose':
      verbose = True
    if o == '-N':
      requested_cores_list += map(int,a.split(':'))


  if verbose:
    print 'This script generates data for the second Level 2 visualization'

  resultsdir = os.path.abspath(resultsdir)
  outputdir = os.path.abspath(outputdir)
  if not title:
    title = os.path.basename(resultsdir)
  title = title.replace(' ', '_')

  try:
    stats = sniper_stats.SniperStats(resultsdir)
    snapshots = stats.get_snapshots()
  except:
    print "No valid results found in "+resultsdir
    sys.exit(1)

  snapshots = sorted([ long(name.split('-')[1]) for name in snapshots if re.match(r'periodic-[0-9]+', name) ])
  defaultinterval = snapshots[1] - snapshots[0]
  defaultnum_intervals = len(snapshots)-1


  if(num_intervals == 0 or num_intervals > defaultnum_intervals):
    print 'No number of intervals specified or number of intervals is to big.'
    print 'Now using all intervals ('+str(defaultnum_intervals)+') found in resultsdir.'
    num_intervals = defaultnum_intervals

  if(interval == 0 or interval < defaultinterval):
    print 'No interval specified or interval is smaller than smallest interval.'
    print 'Now using smallest interval ('+str(defaultinterval)+' femtoseconds).'
    interval = defaultinterval

  if(interval*num_intervals > defaultinterval*defaultnum_intervals):
    print 'The combination '+str(num_intervals)+' intervals and an interval size of '+str(interval)+' is invalid.'
    print 'Now using all intervals ('+str(defaultnum_intervals)+') with the smallest interval size ('+str(defaultinterval)+' femtoseconds).'
    interval = defaultinterval
    num_intervals = defaultnum_intervals


  createJSONData(defaultinterval, defaultnum_intervals, interval, num_intervals, resultsdir, outputdir, title, use_mcpat, verbose = verbose, requested_cores_list = requested_cores_list)

  # Now copy all static files as well
  if outputdir != HOME:
    print "Copy files to output directory "+outputdir
    os.system('cd "%s"; tar c index.html rickshaw/ levels/level2/*html css/ levels/level2/css levels/level2/javascript/ | tar x -C %s' % (HOME, outputdir))
  print "Visualizations can be viewed in "+os.path.join(outputdir,'index.html')


