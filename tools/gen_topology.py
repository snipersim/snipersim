#!/usr/bin/env python

import sys, os, collections, sniper_lib, sniper_stats, sniper_config, getopt


def gen_topology(resultsdir = '.', jobid = None, outputobj = sys.stdout, format = 'svg', embedded = False):
  names = ('hwcontext', 'smt', 'L1-I', 'L1-D', 'L2', 'L3', 'L4', 'tag-dir', 'nuca-cache', 'dram-cache', 'dram-cntlr')
  ids = dict([ (name, collections.defaultdict(lambda: None)) for name in names ])

  stats = sniper_stats.SniperStats(resultsdir)
  config = sniper_config.parse_config(open(os.path.join(resultsdir,'sim.cfg')).read())

  max_id = 0
  for name, lid, mid in stats.get_topology():
    if name not in names:
      print >> sys.stderr, 'Unknown component', name
      continue
    ids[name][int(lid)] = int(mid)
    max_id = max(max_id, int(lid))


  def format_config(name, lid):
    caches = {'L1-I': 'l1_icache', 'L1-D': 'l1_dcache', 'L2': 'l2_cache', 'L3': 'l3_cache', 'L4': 'l4_cache'}
    if name in caches:
      value = sniper_config.get_config(config, 'perf_model/%s/cache_size' % caches[name], lid)
      return sniper_lib.format_size(1024 * long(value), digits = 0)
    elif name == 'dram-cache':
      value = sniper_config.get_config(config, 'perf_model/dram/cache/cache_size', lid)
      return sniper_lib.format_size(1024 * long(value), digits = 0)
    else:
      return ''


  if format == 'text':
    print >> outputobj, ' '*20,
    for lid in range(max_id+1):
      print >> outputobj, '%3d' % lid,
    print >> outputobj

    for name in names:
      if ids[name].keys():
        print >> outputobj, '%-20s' % name,
        for lid in range(max_id+1):
          mid = ids[name][lid]
          if mid is None:
            value = ' '
          elif mid == lid:
            value = 'X'
          else:
            value = '<'
          print >> outputobj, '%3s' % value,
        print >> outputobj


  elif format == 'svg':
    margin_x = 50; step_x = 110
    margin_y = 50; step_y = 50
    items = []
    def paint_init(w, h):
      if not embedded:
        print >> outputobj, '''\
<?xml version="1.0" standalone="no"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
'''
      print >> outputobj, '''\
<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">
<g style="stroke-width:.025in; fill:none">
  ''' % (2*margin_x + w * step_x, 2*margin_y + h * step_y)
    def paint_box((x, y), (w, h), name = '', label = 0, color = '#ffffff', zorder = 0):
      svg = '''\
<rect id="%s" x="%d" y="%d" width="%d" height="%d" rx="0"
   style="stroke:#000000;stroke-width:1;stroke-linejoin:miter; stroke-linecap:butt;fill:%s;"/>
  ''' % (name, margin_x + x * step_x, margin_y + y * step_y, (w - .2) * step_x, (h - .2) * step_y, color)
      if label:
        svg += '''\
<text xml:space="preserve" x="%d" y="%d" fill="#000000"  font-family="Times" font-style="normal" font-weight="normal" font-size="12" text-anchor="start">%s</text>
  ''' % (margin_x + (x + .1) * step_x, margin_y + (y + .3) * step_y, label)
      items.append((zorder, svg))
    def paint_fini():
      for order, svg in sorted(items, reverse = True):
        print >> outputobj, svg
      print >> outputobj, '''\
</g>
</svg>
  '''

    paint_init(max_id+3, len(names)+1)
    xpos = range(max_id+1)
    if ids['smt'].keys():
      x = -1
      for lid in range(max_id+1):
        if ids['smt'][lid] == lid:
          x += 1
        xpos[lid] = x
      ypos = [ 0 for _ in range(max_id+1) ]
      for lid in range(max_id+1):
        mid = ids['smt'][lid]
        paint_box((xpos[lid]+.05, ypos[mid]+.1), (1-.1, 1-.2), 'core-%d' % lid, 'Core #%d' % lid)
        ypos[mid] += .7
      for lid in range(max_id+1):
        if ids['smt'][lid] == lid:
          paint_box((xpos[lid], 0), (1, ypos[mid] + .3), 'smt-%d' % lid, color = '#cccccc', zorder = 1)
      y = max(ypos) + .3
    else:
      for lid in range(max_id+1):
        paint_box((lid, 0), (1, 1), 'core-%d' % lid, 'Core #%d' % lid)
      y = 1
    for name in names:
      if name in ('hwcontext', 'smt'): continue
      if ids[name]:
        size = 0
        for lid in range(max_id, -1, -1):
          if not ids['smt'] or ids['smt'][lid] == lid:
            size += 1
          if ids[name][lid] == lid:
            cfg = format_config(name, lid)
            if cfg:
              label = '%s (%s)' % (name, cfg)
            else:
              label = name
            paint_box((xpos[lid], y), (size, 1), '%s-%d' % (name, lid), label)
            if name == 'dram-cntlr':
              paint_box((xpos[lid]-.075, -.2), (size+.15, y+1+.4), color = '#dddddd', zorder = 2)
            size = 0
        y += 1
    paint_fini()


if __name__ == '__main__':
  outputfilename = None
  formatdefaultoutputfile = {'svg': 'topo.svg', 'text': 'topo.txt'}
  validformats = ('svg', 'text')
  format = 'svg'

  def usage():
    print 'Usage: %s [-h|--help (help)]  [-o|--output (output filename/"-" for stdout)]  [-f|--format (options: %s)]' % (sys.argv[0], validformats)

  try:
    opts, args = getopt.getopt(sys.argv[1:], "ho:f:", [ "help", "output=", "format=" ])
  except getopt.GetoptError, e:
    print e
    usage()
    sys.exit()
  for o, a in opts:
    if o == '-h' or o == '--help':
      usage()
      sys.exit()
    elif o == '-o' or o == '--output':
      outputfilename = a
    elif o == '-f' or o == '--format':
      if a not in validformats:
        print >> sys.stderr, '%s is not a valid format' % a
        usage()
        sys.exit()
      format = a
    else:
      usage()
      sys.exit()

  if outputfilename == None:
    outputfilename = formatdefaultoutputfile[format]

  if outputfilename == '-':
    output = sys.stdout
  else:
    output = open(outputfilename, 'w')

  gen_topology(resultsdir = '.', outputobj = output, format = format)
