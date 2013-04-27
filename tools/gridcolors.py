_jet_data =   {'red':   ((0., 0, 0), (0.35, 0, 0), (0.66, 1, 1), (0.89,1, 1), (1, 0.5, 0.5)),
               'green': ((0., 0, 0), (0.125,0, 0), (0.375,1, 1), (0.64,1, 1), (0.91,0,0), (1, 0, 0)),
               'blue':  ((0., 0.5, 0.5), (0.11, 1, 1), (0.34, 1, 1), (0.65,0, 0), (1, 0, 0))}

# a nice white - blue - red scale
_my_data = {'red':   ((0,1), (.5,.5), (1,1)),
            'green': ((0,1), (.5,.5), (1,.25)),
            'blue':  ((0,1), (.5,1), (1,.25))}

# with linear gray-scale value, so grayscale(this picture) has good scale
#_my_data = {'red':   ((0,1), (.5,.5), (1,.85)),
#            'green': ((0,1), (.5,.5), (1,0)),
#            'blue':  ((0,1), (.5,.75), (1,0))}

def IP(a, b, f): return f*b + (1-f)*a
def IPJ(a, x):
  for i in range(1, len(a)):
    if x < a[i][0]:
      return int(255*IP(a[i-1][1], a[i][1], (x-a[i-1][0])/(a[i][0]-a[i-1][0])))
  return int(255*a[-1][1])
def colorscale(x, scale = _jet_data):
  x = min(1, max(0, x))
  return tuple([ IPJ(_jet_data[c], x) for c in ('red', 'green', 'blue') ])

def grayscale(x):
  x = 1 - x
  return (255*x, 255*x, 255*x)
