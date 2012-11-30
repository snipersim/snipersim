import collections, buildstack, colorsys

def color_tint_shade(base_color, num):
  base_color = map(lambda x:float(x)/255, base_color)
  base_color = colorsys.rgb_to_hsv(*base_color)
  colors = []
  delta = 0.6 / float((num/2) or 1)
  shade = 1.0
  for _ in range(num/2):
    shade -= delta
    colors.append((base_color[0],base_color[1],shade))
  colors = colors[::-1] # Reverse
  if num % 2 == 1:
    colors.append(base_color)
  tint = 1.0
  for _ in range(num/2):
    tint -= delta
    colors.append((base_color[0],tint,base_color[2]))
  colors = map(lambda x:colorsys.hsv_to_rgb(*x),colors)
  colors = map(lambda x:tuple(map(lambda y:int(y*255),x)),colors)
  return colors


def get_colors(plot_labels_ordered, cpiitems):
    contribution_counts = collections.defaultdict(int)
    for i in plot_labels_ordered:
      contribution_counts[cpiitems.names_to_contributions[i]] += 1
    color_ranges = {}
    next_color_index = {}
    for name, color, _ in cpiitems.groups:
      color_ranges[name] = color_tint_shade(color, contribution_counts[name])
      next_color_index[name] = 0
    def get_next_color(contr):
      idx = next_color_index[contr]
      next_color_index[contr] += 1
      return color_ranges[contr][idx]
    return map(lambda x:get_next_color(cpiitems.names_to_contributions[x]),plot_labels_ordered)


class CpiResults:
  def __init__(self, cpidata, cpiitems, no_collapse = False):
    self.cpidata = cpidata
    self.cpiitems = cpiitems
    self.results = buildstack.merge_items(cpidata.data, cpiitems.items, nocollapse = no_collapse)
    self.max_cycles = cpidata.cycles_scale[0] * max(cpidata.times)
    if not self.max_cycles:
      raise ValueError('No cycles accounted during interval')
    # Create an ordered list of all labels used
    labels = set()
    for core, (res, total, other, scale) in self.results.items():
      for name, value in res:
        labels.add(name)
    self.labels = [ label for label in self.cpiitems.names if label in labels ]
    # List of all cores used
    self.cores = self.cpidata.cores

  def get_data(self, metric = 'cpi'):
    data = {}

    for core, (res, total, other, scale) in self.results.items():
      data[core] = {}
      for name, value in res:
        if metric == 'cpi':
          data[core][name] = float(value) / (self.cpidata.instrs[core] or 1)
        elif metric == 'abstime':
          data[core][name] = self.cpidata.fastforward_scale * (float(value) / self.cpidata.cycles_scale[0]) / 1e15 # cycles to femtoseconds to seconds
        elif metric == 'time':
          data[core][name] = float(value) / self.max_cycles
        else:
          raise ValueError('Invalid metric %s' % metric)

    # Make sure all labels exist in all data entries
    for label in self.labels:
      for core in self.cpidata.cores:
        data[core].setdefault(label, 0.0)

    return data

  def get_colors(self):
    return zip(self.labels, get_colors(self.labels, self.cpiitems))
