import collections

class DefaultValue:
  def __init__(self, value):
    self.val = value
  def __call__(self):
    return self.val

# Parse sim.cfg, read from file or from ic.job_output(jobid, 'sim.cfg'), into a dictionary
def parse_config(simcfg):
  import ConfigParser, cStringIO
  cp = ConfigParser.ConfigParser()
  cp.readfp(cStringIO.StringIO(str(simcfg)))
  cfg = {}
  for section in cp.sections():
    for key, value in sorted(cp.items(section)):
      # Remove comments at the end of a line
      value = value.split('#')[0]
      # Run through items sorted by key, so the default comes before the array one
      # Then cut off the [] array markers as they are only used to prevent duplicate option names which ConfigParser doesn't handle
      if key.endswith('[]'):
        key = key[:-2]
      if len(value) > 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
      key = '/'.join((section, key))
      if key in cfg:
        defval = cfg[key]
        cfg[key] = collections.defaultdict(DefaultValue(defval))
        for i, v in enumerate(value.split(',')):
          if v: # Only fill in entries that have been provided
            cfg[key][i] = v
      else: # If there has not been a default value provided, require all array data be populated
        if ',' in value:
          cfg[key] = []
          for i, v in enumerate(value.split(',')):
            cfg[key].append(v)
        else:
          cfg[key] = value
  return cfg


def get_config(config, key, index = None):
  is_hetero = (type(config[key]) == collections.defaultdict)
  if index is None:
    if is_hetero:
      return config[key].default_factory()
    else:
      return config[key]
  elif is_hetero:
    return config[key][index]
  else:
    return config[key]


def get_config_bool(config, key, index = None):
  value = get_config(config, key, index)
  if value.lower() in ('true', 'yes', '1'):
    return True
  elif value.lower() in ('false', 'no', '0'):
    return False
  else:
    raise ValueError('Invalid value for bool %s' % value)


def get_config_default(config, key, defaultval, index = None):
  if key in config:
    return get_config(config, key, index)
  else:
    return defaultval
