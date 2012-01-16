import sys, os

if 'BENCHMARKS_ROOT' in os.environ:
  sys.path.append(os.path.join(os.environ['BENCHMARKS_ROOT'], 'tools', 'scheduler'))
  try:
    from graphite_tools import *
  except:
    sys.stderr.write('Cannot find graphite_tools. Make sure BENCHMARKS_ROOT is set correctly.\n')
    raise
else:
  try:
    from graphite_tools_local import *
  except:
    sys.stderr.write('Cannot find graphite_tools. Either set BENCHMARKS_ROOT, or make sure graphite_tools_local is available.\n')
    raise
