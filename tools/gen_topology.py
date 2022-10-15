#!/usr/bin/env python2

import sys, os, math, collections, sniper_lib, sniper_stats, sniper_config, getopt


def gen_topology(resultsdir = '.', jobid = None, outputobj = sys.stdout, format = 'svg', embedded = False):
  names = ('hwcontext', 'smt', 'L1-I', 'L1-D', 'L2', 'L3', 'L4', 'tag-dir', 'nuca-cache', 'dram-cache', 'dram-cntlr')
  ids = dict([ (name, collections.defaultdict(lambda: None)) for name in names ])

  stats = sniper_stats.SniperStats(resultsdir, jobid)
  config = sniper_lib.get_config(resultsdir = resultsdir, jobid = jobid)

  try:
    topology = stats.get_topology()
  except:
    print >> sys.stderr, "Failed getting topology information"
    topology = None


  max_id = 0
  if topology:
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
    class Svg:
      def __init__(self):
        self.margin_x = 50; self.step_x = 110
        self.margin_y = 50; self.step_y = 50
        self.size_x = 0; self.size_y = 0
        self.items = []
      def paint_box(self, (x, y), (w, h), name = '', label = 0, color = '#ffffff', zorder = 0, margin = (.2, .2), root = (0, 0)):
        x += root[0]; y += root[1]
        self.size_x = max(self.size_x, (x+w) * self.step_x); self.size_y = max(self.size_y, (y+h) * self.step_y)
        svg = '''\
<rect id="%s" x="%d" y="%d" width="%d" height="%d" rx="0"
   style="stroke:#000000;stroke-width:1;stroke-linejoin:miter; stroke-linecap:butt;fill:%s;"/>
  ''' % (name, self.margin_x + x * self.step_x, self.margin_y + y * self.step_y,
               (w - margin[0]) * self.step_x, (h - margin[1]) * self.step_y, color)
        if label:
          svg += '''\
<text xml:space="preserve" x="%d" y="%d" fill="#000000"  font-family="Times" font-style="normal" font-weight="normal" font-size="12" text-anchor="start">%s</text>
  ''' % (self.margin_x + (x + .1) * self.step_x, self.margin_y + (y + .3) * self.step_y, label)
        self.items.append((zorder, svg))
      def write(self, outputobj):
        if not embedded:
          print >> outputobj, '''\
<?xml version="1.0" standalone="no"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
'''
        print >> outputobj, '''\
<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">
<g style="stroke-width:.025in; fill:none">
  ''' % (self.size_x + 2*self.margin_x, self.size_y + 2*self.margin_y)
        for order, svg in sorted(self.items, reverse = True):
          print >> outputobj, svg
        print >> outputobj, '''\
</g>
</svg>
  '''

    svg = Svg()
    ymax = None


    is_mesh = (sniper_config.get_config(config, 'network/memory_model_1') == 'emesh_hop_by_hop')
    if is_mesh:
      ncores = int(config['general/total_cores'])
      dimensions = int(sniper_config.get_config(config, 'network/emesh_hop_by_hop/dimensions'))
      concentration = int(sniper_config.get_config(config, 'network/emesh_hop_by_hop/concentration'))
      if dimensions == 1:
        width, height = int(math.ceil(1.0 * ncores / concentration)), 1
      else:
        if config.get('network/emesh_hop_by_hop/size'):
          width, height = map(int, sniper_config.get_config(config, 'network/emesh_hop_by_hop/size').split(':'))
        else:
          width = int(math.sqrt(ncores / concentration))
          height = int(math.ceil(1.0 * ncores / concentration / width))
      assert width * height * concentration == ncores

      def lid_tile_root(lid):
        return lid - lid % concentration

      def xpos(lid):
        return _xpos[lid] - _xpos[lid_tile_root(lid)]

      def tile_root(lid):
        return ((concentration + .25) * (int(lid / concentration) % width), (ymax + 1) * (lid / concentration / width))

    else:
      def xpos(lid):
        return _xpos[lid]

      def tile_root(lid):
        return (0, 0)


    if topology:
      _xpos = range(max_id+1)
      if ids['smt'].keys():
        x = -1
        for lid in range(max_id+1):
          if ids['smt'][lid] == lid:
            x += 1
          _xpos[lid] = x
        ypos = [ 0 for _ in range(max_id+1) ]
        for lid in range(max_id+1):
          mid = ids['smt'][lid]
          ypos[mid] += .7
        y = max(ypos) + .3
      else:
        y = 1
      ymin = y
      for name in names:
        if name in ('hwcontext', 'smt'): continue
        if ids[name]:
          y += 1
      ymax = y
      y = ymin
      if ids['smt'].keys():
        for lid in range(max_id+1):
          svg.paint_box((xpos(lid)+.05, ypos[mid]+.1), (1-.1, 1-.2), 'core-%d' % lid, 'Core #%d' % lid, root = tile_root(lid))
        for lid in range(max_id+1):
          if ids['smt'][lid] == lid:
            svg.paint_box((xpos(lid), 0), (1, ypos[mid] + .3), 'smt-%d' % lid, color = '#cccccc', zorder = 1, root = tile_root(lid))
      else:
        for lid in range(max_id+1):
          svg.paint_box((xpos(lid), 0), (1, 1), 'core-%d' % lid, 'Core #%d' % lid, root = tile_root(lid))
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
              if is_mesh:
                size = min(size, concentration)
              svg.paint_box((xpos(lid), y), (size, 1), '%s-%d' % (name, lid), label, root = tile_root(lid))
              if name == 'dram-cntlr' and not is_mesh:
                svg.paint_box((xpos(lid)-.075, -.2), (size+.15, y+1+.4), color = '#dddddd', zorder = 2, root = tile_root(lid))
              size = 0
          y += 1
      if is_mesh:
        for lid in range(0, max_id+1, concentration):
          svg.paint_box((xpos(lid)-.075, -.2), (concentration+.15, y+.4), color = '#dddddd', zorder = 2, root = tile_root(lid))
      y += 1
      if is_mesh:
        y *= height
    else:
      y = 0


    if is_mesh:

      results = sniper_lib.get_results(resultsdir = resultsdir, jobid = jobid)['results']
      if 'dram-queue.total-time-used' in results \
         and 'network.shmem-1.mesh.link-up.num-requests' in results:
        import gridcolors

        time0 = max(results['performance_model.elapsed_time'])

        def util2color(utilization):
          return '#%02x%02x%02x' % gridcolors.colorscale(utilization)

        OFFSET_Y = y
        SCALE_X = .6
        BOXSIZE = .2
        for y in range(height):
          for x in range(width):
            for c in range(concentration):
              if c > 0:
                continue
              core = (y * width + x) * concentration + c
              svg.paint_box((x*SCALE_X, y+OFFSET_Y), (SCALE_X*.8, 1*.8), 'node-%d-%d' % (x, y), margin = (0, 0))
              for link, _x, _y in (
                ('down', 0, -.5), ('up', 0, .5),
                ('left', -.5, 0), ('right', .5, 0),
                ('out', -.15, -.2), ('in', .15, -.2),
              ):
                res = results['network.shmem-1.mesh.link-%s.num-requests' % link]
                if core < len(res) and res[core] > 0:
                  utilization = results['network.shmem-1.mesh.link-%s.total-time-used' % link][core] / float(time0)
                  color = util2color(utilization)
                  svg.paint_box(((x + (.5 + _x)*.8 - BOXSIZE/2.)*SCALE_X, y + (.5 + _y)*.8 - BOXSIZE/2. + OFFSET_Y), (BOXSIZE*SCALE_X, BOXSIZE), 'link-%d-%d-%s' % (x, y, link), color = color, zorder = -1, margin = (0, 0))
              if results['dram-queue.num-requests'][core] > 0:
                utilization = results['dram-queue.total-time-used'][core] / float(time0)
                color = util2color(utilization)
                _x, _y = 0, .15
                svg.paint_box(((x + (.5 + _x)*.8 - 1.5*BOXSIZE/2.)*SCALE_X, y + (.5 + _y)*.8 - BOXSIZE/2. + OFFSET_Y), (1.5*BOXSIZE*SCALE_X, BOXSIZE), 'link-%d-%d-%s' % (x, y, link), color = color, zorder = -1, margin = (0, 0))


    svg.write(outputobj)



if __name__ == '__main__':
  resultsdir = '.'
  jobid = None
  outputfilename = None
  formatdefaultoutputfile = {'svg': 'topo.svg', 'text': 'topo.txt'}
  validformats = ('svg', 'text')
  format = 'svg'

  def usage():
    print 'Usage: %s [-h|--help (help)]  [-d <resultsdir (.)> | -j <jobid>]  [-o|--output (output filename/"-" for stdout)]  [-f|--format (options: %s)]' % (sys.argv[0], validformats)

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hd:j:o:f:", [ "help", "output=", "format=" ])
  except getopt.GetoptError, e:
    print e
    usage()
    sys.exit()
  for o, a in opts:
    if o == '-h' or o == '--help':
      usage()
      sys.exit()
    elif o == '-d':
      resultsdir = a
    elif o == '-j':
      jobid = long(a)
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

  gen_topology(resultsdir = resultsdir, jobid = jobid, outputobj = output, format = format)
