import sys, subprocess

def make_stacked_bargraph(outfile, titles, data, ylabel = 'Percent of Cycles', size = (640, 480), title = ''):

  gnuplot_cmd_list = []

  header = '''\
set terminal png font "FreeSans,10" size %d,%d
set output "%s.png"
set boxwidth 0.75 absolute
set style fill solid 1.00 border -1
set style histogram rowstacked
set style data histograms
set key invert reverse Left outside
set mxtics 2
set mytics 2
set ylabel "%s"
set xlabel "Core"
''' % (size[0], size[1], outfile, ylabel)
  gnuplot_cmd_list.append(header)

  if title:
    gnuplot_cmd_list.append('set title "%s"\n' % title)

  cmd = []
  for i in range(2,len(titles)+2):
    (title, color) = titles[i-2]
    if i == len(titles)+1:
      xtic = ':xtic(1)'
    else:
      xtic = ''
    cmd.append(''''-' using %s %s lc %s t "%s"''' % (i,xtic,color,title))
  cmd = 'plot ' + ', '.join(cmd) + '\n'
  gnuplot_cmd_list.append(cmd)

  for i in range(0,len(titles)):
    for c in data.keys():
      gnuplot_cmd_list.append('"%s" ' % c)
      for (t,_) in titles:
        gnuplot_cmd_list.append('%s ' % data[c].get(t, 0.0))
      gnuplot_cmd_list.append('\n')
    gnuplot_cmd_list.append('e\n')

  # For debugging
  #f = open('%s.gnuplot' % outfile, "w")
  #f.write(''.join(gnuplot_cmd_list))

  cmd = ['gnuplot', '-']
  try:
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.PIPE)
    out, err = p.communicate(''.join(gnuplot_cmd_list))
  except OSError:
    print "Warning: Unable to run gnuplot to create cpi stack graphs.  Maybe gnuplot is not installed?"

