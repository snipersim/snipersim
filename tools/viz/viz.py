#!/usr/bin/env python
import os, sys, getopt, re, math, subprocess
HOME = os.path.abspath(os.path.dirname(__file__))
sys.path.extend([ os.path.abspath(os.path.join(HOME, '..')) ])
import sniper_lib, sniper_stats, cpistack, level2, level3


# From http://stackoverflow.com/questions/600268/mkdir-p-functionality-in-python
def mkdir_p(path):
  import errno
  try:
    os.makedirs(path)
  except OSError, exc:
    if exc.errno == errno.EEXIST and os.path.isdir(path):
      pass
    else: raise


if __name__ == '__main__':
  def usage():
    print 'Usage: '+sys.argv[0]+ ' [-h|--help (help)] [-d <resultsdir (default: .)>] [-t <title>] [-n <num-intervals (default: 1000, all: 0)>] [-i <interval (default: smallest_interval)>] [-o <outputdir>] [--mcpat] [-v|--verbose]'
    sys.exit()

  resultsdir = '.'
  outputdir = '.'
  title = None
  use_mcpat = False
  num_intervals = 1000
  interval = None
  verbose = False

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hd:o:t:n:i:v", [ "help", "mcpat", "verbose" ])
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
    if o == '--mcpat':
      use_mcpat = True
    if o == '-t':
      title = a
    if o == '-n':
      num_intervals = long(a)
    if o == '-i':
      interval = long(a)
    if o == '-v' or o == '--verbose':
      verbose = True


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

  # Check if number of intervals and interval size are valid

  if num_intervals == 0:
    num_intervals = defaultnum_intervals
  elif num_intervals <= 0:
    print 'Number of intervals is invalid (%s), using (%s) intervals instead.' % (num_intervals, defaultnum_intervals)
    num_intervals = defaultnum_intervals
  elif num_intervals > defaultnum_intervals:
    print 'Number of intervals is too large (%s), using (%s) intervals instead.' % (num_intervals, defaultnum_intervals)
    num_intervals = defaultnum_intervals

  if interval == None:
    interval = defaultinterval
  elif interval <= 0:
    print 'Invalid interval specified (%s), using the smallest interval (%s) instead' % (interval, defaultinterval)
    interval = defaultinterval
  elif interval < defaultinterval:
    print 'Interval is smaller than the smallest interval, using the smallest interval (%s) instead.' % defaultinterval
    interval = defaultinterval

  if(interval*num_intervals > defaultinterval*defaultnum_intervals):
    print 'The combination '+str(num_intervals)+' intervals and an interval size of '+str(interval)+' is invalid.'
    print 'Now using all intervals ('+str(defaultnum_intervals)+') with the smallest interval size ('+str(defaultinterval)+' femtoseconds).'
    interval = defaultinterval
    num_intervals = defaultnum_intervals

  # Mainline visualization

  mkdir_p(outputdir)

  level2.createJSONData(interval, num_intervals, resultsdir, outputdir, title, use_mcpat, verbose = verbose)
  level3.createJSONData(interval, num_intervals, resultsdir, outputdir, title, verbose = verbose)

  if verbose:
    print "Write general info about the visualizations in info.txt"
  info = open(os.path.join(outputdir,'info.txt'), "w")
  info.write("title = '"+title+"';\n")
  info.write("num_intervals = '"+str(num_intervals)+"';\n")
  info.write("interval = '"+str(interval)+"';\n")
  info.write("use_mcpat = '"+str(use_mcpat)+"';\n")
  info.close()

  # Now copy all static files as well
  if outputdir != HOME:
    if verbose:
      print "Copy files to output directory "+outputdir
    os.system('cd "%s"; tar c visualization.html rickshaw/ levels/level2/*html levels/level3/*html css/ levels/level2/css levels/level2/javascript/ levels/level3/javascript | tar x -C %s' % (HOME, outputdir))
  if verbose:
    print "Visualizations can be viewed in "+os.path.join(outputdir,'visualization.html')
