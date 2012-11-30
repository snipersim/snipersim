import buildstack

class CpiResults:
  def __init__(self, cpidata, cpiitems, no_collapse = False):
    self.cpidata = cpidata
    self.cpiitems = cpiitems
    self.results = buildstack.merge_items(cpidata.data, cpiitems.items, nocollapse = no_collapse)
    self.max_cycles = cpidata.cycles_scale[0] * max(cpidata.times)
    if not self.max_cycles:
      raise ValueError('No cycles accounted during interval')

  def get_data(self, metric = 'cpi'):
    labels = set()
    data = {}

    for core, (res, total, other, scale) in self.results.items():
      data[core] = {}
      for name, value in res:
        labels.add(name)
        if metric == 'cpi':
          data[core][name] = float(value) / (self.cpidata.instrs[core] or 1)
        elif metric == 'abstime':
          data[core][name] = self.cpidata.fastforward_scale * (float(value) / self.cpidata.cycles_scale[0]) / 1e15 # cycles to femtoseconds to seconds
        elif metric == 'time':
          data[core][name] = float(value) / self.max_cycles
        else:
          raise ValueError('Invalid metric %s' % metric)

    # Create an ordered list of labels that is the superset of all labels used from all cores
    labels = [ label for label in self.cpiitems.names if label in labels ]

    # Make sure all labels exist in all data entries
    for label in labels:
      for core in self.cpidata.cores:
        data[core].setdefault(label, 0.0)

    return labels, self.cpidata.cores, data
