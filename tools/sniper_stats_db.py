import os, bsddb, struct, zlib

class SniperStatsDbObject:
  def __init__(self, data):
    self.data = zlib.decompress(data)
    self.offset = 0
  def end(self):
    return self.offset >= len(self.data)
  def read_int32(self):
    (value,) = struct.unpack_from("i", self.data, self.offset)
    self.offset += 4
    return value
  def read_uint64(self):
    (value,) = struct.unpack_from("L", self.data, self.offset)
    self.offset += 8
    return value
  def read_string(self):
    size = self.read_int32()
    (value,) = struct.unpack_from("%ds" % size, self.data, self.offset)
    self.offset += size
    return value


class SniperStatsDb:
  def __init__(self, filename = 'sim.stats.db'):
    self.db = bsddb.hashopen(filename, 'r')
    self.names = self.read_metricnames()

  def get_snapshots(self):
    return [ key[1:] for key in self.db.keys() if key.startswith('d') ]

  def read_metricnames(self):
    names = {}
    data = SniperStatsDbObject(self.db['k'])
    while not data.end():
      keyid = data.read_int32()
      object = data.read_string()
      metric = data.read_string()
      names[keyid] = (object, metric)
    return names

  def read_snapshot(self, prefix):
    values = {}
    data = SniperStatsDbObject(self.db['d%s' % prefix])
    num = data.read_int32()
    while not data.end():
      metricid = data.read_int32()
      items = {}
      while True:
        index = data.read_int32()
        if index == -12345: break
        value = data.read_uint64()
        items[index] = value
      values[metricid] = items
    return values

  def parse_stats(self, (k1, k2), ncores):
    v1 = self.read_snapshot(k1)
    v2 = self.read_snapshot(k2)
    results = []
    for metricid, list in v2.items():
      name = '%s.%s' % self.names[metricid]
      for idx in range(min(0, min(list.keys() or [0])), max(ncores, max(list.keys() or [0])+1)):
        val1 = v1.get(metricid, {}).get(idx, 0)
        val2 = v2.get(metricid, {}).get(idx, 0)
        results.append((name, idx, val2 - val1))
        if name == 'performance_model.elapsed_time' and idx < ncores:
          results.append(('performance_model.elapsed_time_begin', idx, val1))
          results.append(('performance_model.elapsed_time_end', idx, val2))
    return results


if __name__ == '__main__':
  stats = SniperStatsDb()
  print stats.get_snapshots()
  names = stats.read_metricnames()
  print stats.read_snapshot('roi-end')
