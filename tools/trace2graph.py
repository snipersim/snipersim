#!/usr/bin/env python2

import sys, Image, ImageDraw

if len(sys.argv) < 2:
  print 'Usage: %s <filename> [<time_min>] [<time_max>]' % sys.argv[0]
  sys.exit(-1)


FILENAME = sys.argv[1]
if len(sys.argv) > 2:
  TSTART = int(sys.argv[2])
else:
  TSTART = 0
if len(sys.argv) > 3:
  TEND = int(sys.argv[3])
else:
  TEND = 0

_TSTART = [] # neutral element for min()
_TEND = None # neutral element for max()
NTHREADS = 0
_ROI_BEGIN = None
_ROI_END = None

for line in file(FILENAME):
  try:
    cpu, time, state = map(long, line.split())
  except ValueError:
    print 'Ignoring invalid line', line
    continue
  NTHREADS = max(NTHREADS, cpu + 1)
  _TSTART = min(_TSTART, time)
  _TEND = max(_TEND, time)
  if state == 5:
    _ROI_BEGIN = time
  elif state == 6:
    _ROI_END = time

if not TSTART: TSTART = _TSTART
if not TEND: TEND = _TEND



WIDTH = 1000
HEIGHT = 400

X = lambda t: WIDTH * (t - TSTART) / (TEND - TSTART)
Y = lambda cpu, state: HEIGHT - 1 - HEIGHT * (cpu * 5 + state) / float(5 * NTHREADS)

im = Image.new("RGB", (WIDTH, HEIGHT), (255,255,255))
draw = ImageDraw.Draw(im)

lasttime = {}
laststate = {}

states = [[ 0 for i in range(NTHREADS) ] for j in range(WIDTH) ]

for line in file(FILENAME):
  try:
    cpu, time, state = map(long, line.split())
  except ValueError:
    print 'Ignoring invalid line', line
    continue
  if state > 3: continue
  if cpu in lasttime:
    t0 = lasttime[cpu]
    s0 = laststate[cpu]
    x0 = X(t0); x1 = X(time)
    if x0 < WIDTH and x1 >= 0:
      for x in range(max(0, x0), min(WIDTH-1, x1)+1):
        states[x][cpu] |= 1 << s0
  lasttime[cpu] = time
  laststate[cpu] = state

for x in range(WIDTH):
  for cpu in range(NTHREADS):
    for s in range(5):
      if states[x][cpu] & (1 << s):
        h, c = { 0: (0, (0,0,255)), 1: (.5, (0,255,0)), 2: (1, (128,128,0)), 3: (1, (255,0,0)) }[s]
        draw.rectangle([(x, Y(cpu, s+h)), (x, Y(cpu, s)-1)], c)

if _ROI_BEGIN and _ROI_END:
  draw.rectangle([(X(_ROI_BEGIN), Y(0, 0)), (X(_ROI_END), Y(0, 0)-2)], 'rgb(255,0,0)')

im.save('trace2graph.png', 'PNG')
