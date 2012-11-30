import sys

def merge_items(data, all_items, nocollapse = False, no_complain_others = False):
  # Recursively walk the contributor list
  # Current list is in <items>, remaining values to be mapped in <values>,
  #   conversion factor to calculate percentages to compare with the threshold in <scale>

  def get_items(prefix, items, values, scale):
    res = []  # Sub-components in this stack
    total = 0 # Total value of this (sub)stack, based on this we'll later decide whether to show this stack as components or aggregate
    other = 0 # Subcomponents that were too small to include in <res>
    for name, threshold, key_or_items in items:
      if type(key_or_items) is list:
        _res, _total, _other = get_items(prefix+name+'-', key_or_items, values, scale)
        total += _total
        if _total / scale <= threshold and not nocollapse:
          # Sub-stack total is below threshold: add to Others
          other += _total
        elif not _res and not nocollapse:
          # Sub-stack has no components above threshold: add aggregate value only
          res.append((prefix+name, _total))
        else:
          # Sub-stack is above threshold: add each component + others component
          res += _res
          other += _other
      else:
        if type(key_or_items) is not tuple:
          key_or_items = (key_or_items,)
        value = 0
        for key in key_or_items:
          if key in values:
            value += values[key]
            # Delete this value so we don't count it with 'Others'
            del values[key]
        total += value
        if value / scale <= threshold and not nocollapse:
          # Value is below threshold: add to others
          other += value
        else:
          # Value is above threshold: add by itself
          res.append((prefix+name, value))
    return res, total, other

  results = {}
  for core, values in data.items():
    scale = float(sum(values.values())) or 1. # Conversion factor from sim.stats values to %, for comparison with threshold
    res, total, other = get_items('', all_items, values, scale)
    # Everything that's left in <values> is unknown
    other += sum(values.values())
    if other:
      res.append(('other', other))
    if values and (not no_complain_others or sum(values.values()) > 0):
      sys.stderr.write('Also found but not in all_items: %s\n' % values)
    results[core] = (res, total, other, scale)
  return results


def get_names(items, prefix = '', add_prefixes = True, keys = None):
  names = []
  for name, threshold, key_or_items in items:
    if not keys or name in keys:
      if type(key_or_items) is list:
        if add_prefixes:
          names.append(name) # Add the top-level name if requested
        names += get_names(key_or_items, name, add_prefixes)
      else:
        if prefix:
          names.append(prefix+'-'+name)
        else:
          names.append(name)
  return names
