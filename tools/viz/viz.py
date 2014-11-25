#!/usr/bin/env python2
import os, sys, getopt, re, math, subprocess, json, shutil
HOME = os.path.abspath(os.path.dirname(__file__))
sys.path.extend([ os.path.abspath(os.path.join(HOME, '..')) ])
import sniper_lib, sniper_stats, cpistack, level1, level2, level3, topology, profile, functionbased


# From http://stackoverflow.com/questions/600268/mkdir-p-functionality-in-python
def mkdir_p(path):
  import errno
  try:
    os.makedirs(path)
  except OSError, exc:
    if exc.errno == errno.EEXIST and os.path.isdir(path):
      pass
    else: raise


levels_all = [ '1', '2', '3', 'topo', 'profile', 'aso' ]
levels_default = [ '1', '2', '3', 'topo' ]


if __name__ == '__main__':
  def usage():
    print 'Usage: '+sys.argv[0]+ ' [-h|--help (help)] [-d <resultsdir (default: .)>] [-j <jobid>] [-t <title>] [-n <num-intervals (default: 1000, all: 0)>] [-i <interval (default: smallest_interval)>] [-o <outputdir (default: viz)>] [--mcpat] [--level <levels (default: %s)>] [--add-level <level>] [-v|--verbose]' % ','.join(levels_default)
    sys.exit()

  resultsdir = '.'
  outputdir = 'viz'
  title = None
  use_mcpat = False
  num_intervals = 1000
  interval = None
  verbose = False
  levels = levels_default
  dircleanup = None

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hd:o:t:n:i:vj:", [ "help", "mcpat", "level=", "add-level=", "verbose" ])
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
    if o == '--level':
      for l in a.split(','):
        if l not in levels_all:
          print 'Invalid level', l
          sys.exit(1)
      levels = a.split(',')
    if o == '--add-level':
      if a not in levels_all:
	print 'Invalid level', a
	sys.exit(1)
      levels.append(a)
    if o == '-v' or o == '--verbose':
      verbose = True
    if o == '-j':
      import tempfile, binascii, iqclient
      jobid = int(a)
      dircleanup = tempfile.mkdtemp()
      tarfn = os.path.join(dircleanup,'%d.tar.gz'%jobid)
      ic = iqclient.IntelClient()
      with open(tarfn, 'w') as fp:
        fp.write(binascii.a2b_base64(ic.job_output(jobid, False)))
      waitrc = os.system('tar -x -z -f "%s" -C "%s" && rm "%s"' % (tarfn, dircleanup, tarfn))
      rc = (waitrc >> 8) & 0xff
      if rc != 0:
        print 'Error: Unable to download and extract data from jobid %d.' % jobid
        shutil.rmtree(dircleanup)
        dircleanup = None
        sys.exit(1)
      resultsdir = dircleanup


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
  elif num_intervals < defaultnum_intervals:
    # Automatically determine interval to end up with (around) num_intervals in total
    interval = defaultinterval * max(1, int(defaultnum_intervals / num_intervals))
    num_intervals = defaultinterval * defaultnum_intervals / interval

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

  if '1' in levels: level1.createJSONData(resultsdir, outputdir, verbose = verbose)
  if '2' in levels: level2.createJSONData(defaultinterval, defaultnum_intervals, interval, num_intervals, resultsdir, outputdir, title, use_mcpat, verbose = verbose)
  if '3' in levels: level3.createJSONData(interval, num_intervals, resultsdir, outputdir, title, verbose = verbose)
  if 'topo' in levels: topology.createJSONData(interval, num_intervals, resultsdir, outputdir, verbose = verbose)
  if 'profile' in levels: profile.createJSONData(resultsdir, outputdir, verbose = verbose)
  if 'aso' in levels: functionbased.createJSONData(resultsdir, outputdir, title)

  if verbose:
    print "Write general info about the visualizations in info.txt"
  info = open(os.path.join(outputdir,'info.txt'), "w")
  info.write("title = '"+title+"';\n")
  info.write("num_intervals = '"+str(num_intervals)+"';\n")
  info.write("interval = '"+str(interval)+"';\n")
  info.write("use_level2 = "+str(1 if '2' in levels else 0)+";\n")
  info.write("use_mcpat = "+str(1 if use_mcpat else 0)+";\n")
  info.write("use_level3 = "+str(1 if '3' in levels else 0)+";\n")
  info.write("use_topo = "+str(1 if 'topo' in levels else 0)+";\n")
  info.write("use_profile = "+str(1 if 'profile' in levels else 0)+";\n")
  info.write("use_aso = "+str(1 if 'aso' in levels else 0)+";\n")
  info.close()

  asoinfo = open(os.path.join(outputdir,'asoinfo.txt'), "w")
  asoinfo.write("asoinfo = '"+json.dumps(dict(use_aso=('aso' in levels)))+"';\n")
  asoinfo.close()

  # Now copy all static files as well
  if outputdir != HOME:
    if verbose:
      print "Copy files to output directory "+outputdir
    os.system('cd "%s"; tar c index.html rickshaw/ levels/level2/*html levels/level3/*html levels/topology/*html levels/profile/*html css/ images/ scripts/ levels/level2/css levels/level2/javascript/ levels/level3/javascript | tar x -C %s' % (HOME, outputdir))
    if 'aso' in levels:
      os.system('cd "%s"; tar c flot/ levels/functionbased/functionbased.html levels/functionbased/*js css/ levels/functionbased/doxygen | tar x -C %s' % (HOME, outputdir))
  if verbose:
    print "Visualizations can be viewed in "+os.path.join(outputdir,'index.html')

  if dircleanup:
    shutil.rmtree(dircleanup)
    dircleanup = None
