#!/usr/bin/env python

import sys, os, collections, sniper_lib


names = ('hwcontext', 'smt', 'L1-I', 'L1-D', 'L2', 'L3', 'L4', 'dram-cache', 'dram-dir', 'dram-cntlr')
ids = dict([ (name, collections.defaultdict(lambda: ' ')) for name in names ])

max_id = 0
for line in open('sim.topo'):
  name, lid, mid = line.split()
  if name not in names:
    print >> sys.stderr, 'Unknown component', name
    continue
  if lid == mid:
    ids[name][int(lid)] = 'X'
  else:
    ids[name][int(lid)] = '<'
  max_id = max(max_id, int(lid))

config = sniper_lib.parse_config(open('sim.cfg').read())


def format_config(name, lid):
  caches = {'L1-I': 'l1_icache', 'L1-D': 'l1_icache', 'L2': 'l2_cache', 'L3': 'l3_cache', 'L4': 'l4_cache'}
  if name in caches:
    value = sniper_lib.get_config(config, 'perf_model/%s/cache_size' % caches[name], lid)
    return sniper_lib.format_size(1024 * long(value), digits = 0)
  elif name == 'dram-cache':
    value = sniper_lib.get_config(config, 'perf_model/dram/cache/cache_size', lid)
    return sniper_lib.format_size(1024 * long(value), digits = 0)
  else:
    return ''


format = 'svg'

if format == 'text':
  print ' '*20,
  for lid in range(max_id+1):
    print '%3d' % lid,
  print

  for name in names:
    if ids[name]:
      print '%-20s' % name,
      for lid in range(max_id+1):
        print '%3s' % ids[name][lid],
      print


elif format == 'svg':
  step_x = 100
  step_y = 50
  def paint_init(w, h):
    print '''\
<?xml version="1.0" standalone="no"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">
<g style="stroke-width:.025in; fill:none">
''' % (w * step_x, h * step_y)
  def paint_box((x, y), (w, h), label = 0):
    print '''\
<rect x="%d" y="%d" width="%d" height="%d" rx="0"
   style="stroke:#000000;stroke-width:1;stroke-linejoin:miter; stroke-linecap:butt;fill:#ffffff;"/>
''' % (x * step_x, y * step_y, (w - .2) * step_x, (h - .2) * step_y)
    if label:
      print '''\
<text xml:space="preserve" x="%d" y="%d" fill="#000000"  font-family="Times" font-style="normal" font-weight="normal" font-size="12" text-anchor="start">%s</text>
''' % ((x + .1) * step_x, (y + .3) * step_y, label)
  def paint_fini():
    print '''\
</g>
</svg>
'''

  paint_init(max_id+3, len(names)+1)
  for i, name in enumerate(names):
    size = 0
    for lid in range(max_id, -1, -1):
      size += 1
      if ids[name][lid] == 'X':
        if name == 'hwcontext':
          label = 'Core #%d' % lid
        elif name == 'smt':
          label = 'Core'
        else:
          cfg = format_config(name, lid)
          if cfg:
            label = '%s (%s)' % (name, cfg)
          else:
            label = name
        paint_box((lid, i), (size, 1), label)
        size = 0
  paint_fini()
