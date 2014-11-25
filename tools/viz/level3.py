#!/usr/bin/env python2
import os, sys, getopt, re, math, subprocess
HOME = os.path.abspath(os.path.dirname(__file__))
sys.path.extend([ os.path.abspath(os.path.join(HOME, '..')) ])
import sniper_lib, sniper_config, sniper_stats, cpistack, json


# From http://stackoverflow.com/questions/600268/mkdir-p-functionality-in-python
def mkdir_p(path):
  import errno
  try:
    os.makedirs(path)
  except OSError, exc:
    if exc.errno == errno.EEXIST and os.path.isdir(path):
      pass
    else: raise

def createJSONData(interval, num_intervals, resultsdir, outputdir, title, verbose = False):
  if verbose:
    print 'Generate JSON data for Level 3'

  stats = sniper_stats.SniperStats(resultsdir)
  config = sniper_config.parse_config(file(os.path.join(resultsdir, 'sim.cfg')).read())

  ncores = int(config['general/total_cores'])
  if verbose:
    print ncores, "cores detected"

  intervaldata = [0 for x in xrange(num_intervals)]
  num_exceptions=0
  for i in range(0,num_intervals):
    if verbose:
      print "Parsing interval "+str(i+1)+"/"+str(num_intervals)+"\r",

    try:
      results = cpistack.cpistack_compute(
        config = config,
        stats = stats,
        partial = ["periodic-"+str(i*interval),"periodic-"+str((i+1)*interval)],
        use_simple = False,
        use_simple_mem = True,
        no_collapse = False,
        aggregate = False
      )
      data = results.get_data('cpi')

      intervaldata[i] = [0 for x in xrange(ncores)]

      for core in xrange(ncores):
        if core in results.cores:
          intervaldata[i][core] = {'time':(i*interval/1000000), 'ipc':1./sum(data[core].itervalues())}
        else:
          intervaldata[i][core] = {'time':(i*interval/1000000), 'ipc':0}

    except ValueError:
      intervaldata[i] = [0 for x in xrange(ncores)]
      for j in range(0,ncores):
        intervaldata[i][j] = dict(time=(i*interval/1000000), ipc=0)
      num_exceptions += 1
      continue

  # Write JSON to file
  mkdir_p(os.path.join(outputdir,'levels','level3','data'))
  f = open(os.path.join(outputdir,'levels','level3','data','ipcvalues.txt'), "w")
  f.write("intervalsize = "+str(interval)+";\n")
  f.write("ipcvaluestr = '"+json.dumps(intervaldata)+"';")
  f.close()
  f = open(os.path.join(outputdir,'levels','level3','data','ipcvalues.json'), "w")
  f.write(json.dumps(intervaldata, indent=4))
  f.close()
  if verbose:
    print
  if(num_exceptions>0):
    if verbose:
      print("There was no useful information for "+str(num_exceptions)+" intervals.")
      print("You might want to increase the interval size.")
  if verbose:
    print('[OK]')


if __name__ == '__main__':
  def usage():
    print 'Usage: '+sys.argv[0]+ ' [-h|--help (help)] [-d <resultsdir (default: .)>] [-t <title>] [-n <num-intervals (default: all_intervals)] [-i <interval (default: smallest_interval)>] [-o <outputdir>] [-v|--verbose]'
    sys.exit()

  resultsdir = '.'
  outputdir = HOME
  title = None
  use_mcpat = False
  num_intervals = 0
  interval = 0
  verbose = False

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hd:o:t:n:i:v", [ "help", "verbose" ])
  except getopt.GetoptError, e:
    print e
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
    if o == '-n':
      num_intervals = long(a)
    if o == '-i':
      interval = long(a)
    if o == '-v' or o == '--verbose':
      verbose = True

  if verbose:
    print 'This script generates data for the Level 3 visualization'

  resultsdir = os.path.abspath(resultsdir)
  outputdir = os.path.abspath(outputdir)
  if not title:
    title = os.path.basename(resultsdir)
  title = title.replace(' ', '_')

  try:
    stats = sniper_stats.SniperStats(resultsdir)
    snapshots = stats.get_snapshots()
  except:
    print "Error, no valid results found in "+resultsdir
    sys.exit(1)

  snapshots = sorted([ long(name.split('-')[1]) for name in snapshots if re.match(r'periodic-[0-9]+', name) ])
  defaultinterval = snapshots[1] - snapshots[0]
  defaultnum_intervals = len(snapshots)-1

  # Check if number of intervals and interval size are valid

  if(num_intervals == 0 or num_intervals > defaultnum_intervals):
    print 'No number of intervals specified or number of intervals is to big.'
    print 'Now using all intervals ('+str(defaultnum_intervals)+') found in resultsdir.'
    num_intervals = defaultnum_intervals

  if(interval == 0 or interval < defaultinterval):
    print 'No interval specified or interval is smaller than smallest interval.'
    print 'Now using smallest interval ('+str(defaultinterval)+' femtoseconds).'
    interval = defaultinterval

  if(interval*num_intervals > defaultinterval*defaultnum_intervals):
    print 'The combination '+str(num_intervals)+' interval and an interval size of '+str(interval)+' is invalid.'
    print 'Now using all intervals ('+str(defaultnum_intervals)+') with the smallest interval size ('+str(defaultinterval)+' femtoseconds).'
    interval = defaultinterval
    num_intervals = defaultnum_intervals

  # Mainline visualization

  mkdir_p(outputdir)

  if verbose:
    print
  createJSONData(interval, num_intervals, resultsdir, outputdir, title, verbose = verbose)
  if verbose:
    print

  # Now copy all static files as well
  if outputdir != HOME:
    print "Copy files to output directory "+outputdir
    os.system('cd "%s"; tar c index.html rickshaw/ levels/level3/*html levels/level3/javascript css/ | tar x -C %s' % (HOME, outputdir))
  print "Visualizations can be viewed in "+os.path.join(outputdir,'index.html')

