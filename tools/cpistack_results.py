import buildstack

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
    return self.cpiitems.get_colors(self.labels)
