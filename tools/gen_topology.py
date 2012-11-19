#!/usr/bin/env python

import sys, os, collections, sqlite3, sniper_lib, sniper_config, getopt

outputfilename = 'topo.svg'
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

if outputfilename == '-':
  output = sys.stdout
else:
  output = open(outputfilename, 'w')

names = ('hwcontext', 'smt', 'L1-I', 'L1-D', 'L2', 'L3', 'L4', 'dram-cache', 'dram-dir', 'dram-cntlr')
ids = dict([ (name, collections.defaultdict(lambda: None)) for name in names ])

max_id = 0
db = sqlite3.connect('sim.stats.sqlite3')
for name, lid, mid in db.execute('SELECT componentname, coreid, masterid FROM topology').fetchall():
  if name not in names:
    print >> sys.stderr, 'Unknown component', name
    continue
  ids[name][int(lid)] = int(mid)
  max_id = max(max_id, int(lid))

config = sniper_config.parse_config(open('sim.cfg').read())


def format_config(name, lid):
  caches = {'L1-I': 'l1_icache', 'L1-D': 'l1_icache', 'L2': 'l2_cache', 'L3': 'l3_cache', 'L4': 'l4_cache'}
  if name in caches:
    value = sniper_config.get_config(config, 'perf_model/%s/cache_size' % caches[name], lid)
    return sniper_lib.format_size(1024 * long(value), digits = 0)
  elif name == 'dram-cache':
    value = sniper_config.get_config(config, 'perf_model/dram/cache/cache_size', lid)
    return sniper_lib.format_size(1024 * long(value), digits = 0)
  else:
    return ''


if format == 'text':
  print >> output, ' '*20,
  for lid in range(max_id+1):
    print >> output, '%3d' % lid,
  print >> output

  for name in names:
    if ids[name].keys():
      print >> output, '%-20s' % name,
      for lid in range(max_id+1):
        mid = ids[name][lid]
        if mid is None:
          value = ' '
        elif mid == lid:
          value = 'X'
        else:
          value = '<'
        print >> output, '%3s' % value,
      print >> output


elif format == 'svg':
  margin_x = 50; step_x = 110
  margin_y = 50; step_y = 50
  items = []
  def paint_init(w, h):
    print >> output, '''\
<?xml version="1.0" standalone="no"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">
<g style="stroke-width:.025in; fill:none">
''' % (2*margin_x + w * step_x, 2*margin_y + h * step_y)
  def paint_box((x, y), (w, h), label = 0, color = '#ffffff', zorder = 0):
    svg = '''\
<rect x="%d" y="%d" width="%d" height="%d" rx="0"
   style="stroke:#000000;stroke-width:1;stroke-linejoin:miter; stroke-linecap:butt;fill:%s;"/>
''' % (margin_x + x * step_x, margin_y + y * step_y, (w - .2) * step_x, (h - .2) * step_y, color)
    if label:
      svg += '''\
<text xml:space="preserve" x="%d" y="%d" fill="#000000"  font-family="Times" font-style="normal" font-weight="normal" font-size="12" text-anchor="start">%s</text>
''' % (margin_x + (x + .1) * step_x, margin_y + (y + .3) * step_y, label)
    items.append((zorder, svg))
  def paint_fini():
    for order, svg in sorted(items, reverse = True):
      print >> output, svg
    print >> output, '''\
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
      paint_box((xpos[lid]+.05, ypos[mid]+.1), (1-.1, 1-.2), 'Core #%d' % lid)
      ypos[mid] += .7
    for lid in range(max_id+1):
      if ids['smt'][lid] == lid:
        paint_box((xpos[lid], 0), (1, ypos[mid] + .3), color = '#cccccc', zorder = 1)
    y = max(ypos) + .3
  else:
    for lid in range(max_id+1):
      paint_box((lid, 0), (1, 1), 'Core #%d' % lid)
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
          paint_box((xpos[lid], y), (size, 1), label)
          if name == 'dram-cntlr':
            paint_box((xpos[lid]-.075, -.2), (size+.15, y+1+.4), color = '#dddddd', zorder = 2)
          size = 0
      y += 1
  paint_fini()
