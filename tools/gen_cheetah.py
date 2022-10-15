#!/usr/bin/env python2

import sys, os, getopt, subprocess, sniper_lib

def generate_cheetah(jobid = None, resultsdir = '.', partial = None, outputbase = 'cheetah', title = None, yscale = (0, 50), logy = False, diff = False):
  res = sniper_lib.get_results(jobid = jobid, resultsdir = resultsdir, partial = partial)
  data = dict([ (k.split('.')[1], v) for k, v in res['results'].items() if k.startswith('cheetah.') ])

  def grouping_sortkey(grouping):
    if grouping == 'global':
      return -99999 # Start with total
    elif grouping == 'local':
      return +99999 # End with per-thread
    else:
      return -int(grouping.split('-')[-1]) # Big groups (close to total) to small groups (close to per-thread)

  def grouping_title(grouping):
    return grouping

  GROUPINGS = sorted(data.keys(), key = grouping_sortkey)
  xmax = 1 << max(map(len, data.values()))

  o = file(outputbase + '.input', 'w')
  o.write('''\
set fontpath "/usr/share/fonts/truetype/freefont"
set terminal png font "FreeSans,15" size 500,350 linewidth 2 rounded
set output "%s.png"
%s
set key top right
set logscale x 2
set xrange [:%f]
set xtics nomirror out ("1 KB" 1024, "32 KB" 32768, "1 MB" 1048576, "32 MB" 33554432, "1 GB" 1073741824, "32 GB" 34359738368.)
set xtics add autofreq
set mxtics default
%s
set yrange [%f:%f]
set ytics nomirror
set format y "%%.1f%%%%"
plot %s
''' % (os.path.basename(outputbase), 'set title "%s"' % title if title else 'unset title', xmax, 'set logscale y' if logy else '', yscale[0], yscale[1],
    ', '.join([ "'-' using 1:(100*$2) with linespoints title '%s'" % grouping_title(grouping) for grouping in GROUPINGS ])
  ))

  for grouping in GROUPINGS:
    last = 1
    total = data[grouping][0]
    for size, value in list(enumerate(data[grouping]))[1:]:
      if value == 0: continue
      value = 1 - value / float(total)
      o.write('%d %f\n' % (1 << size, last-value if diff else value))
      last = value
    o.write('e\n')

  del o
  subprocess.Popen([ 'gnuplot', '%s.input' % os.path.basename(outputbase) ], cwd = os.path.dirname(outputbase) or '.').communicate()



if __name__ == '__main__':
  def usage():
    print 'Usage:', sys.argv[0], '[-h (help)] [-j <jobid> | -d <resultsdir (default: .)>] [--partial=<begin:end (roi-begin:roi-end)>] [-o <output (cheetah)>] [-t <title>] [-y <ymax> | --logy=<miny:maxy>] [--diff]'
    sys.exit(-1)

  jobid = 0
  resultsdir = '.'
  partial = None
  outputbase = 'cheetah'
  title = None
  yscale = 0, 50
  logy = False
  diff = False

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hj:d:o:t:y:", [ 'partial=', 'logy=', 'diff' ])
  except getopt.GetoptError, e:
    print e
    usage()
  for o, a in opts:
    if o == '-h':
      usage()
    if o == '-d':
      resultsdir = a
    if o == '-j':
      jobid = long(a)
    if o == '-o':
      outputbase = a
    if o == '--partial':
      if ':' not in a:
        sys.stderr.write('--partial=<from>:<to>\n')
        usage()
      partial = a.split(':')
  if o == '-t':
    title = a
  if o == '-y':
    yscale = 0, float(a)
  if o == '--logy':
    yscale = map(float, a.split(':'))
    logy = True
  if o == '--diff':
    diff = True

  generate_cheetah(jobid = jobid, resultsdir = resultsdir, outputbase = outputbase, partial = partial, title = title, yscale = yscale, logy = logy, diff = diff)
