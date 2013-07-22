import os, sys, json, collections, distutils.spawn
HOME = os.path.abspath(os.path.dirname(__file__))
sys.path.extend( [os.path.abspath(os.path.join(HOME, '..'))] )
import gen_profile, sniper_lib, sniper_config, sniper_stats


# From http://stackoverflow.com/questions/600268/mkdir-p-functionality-in-python
def mkdir_p(path):
  import errno
  try:
    os.makedirs(path)
  except OSError, exc:
    if exc.errno == errno.EEXIST and os.path.isdir(path):
      pass
    else: raise


def createJSONData(resultsdir, outputdir, verbose = False):
  profiledir = os.path.join(outputdir,'levels','profile')
  mkdir_p(profiledir)

  prof = gen_profile.Profile(resultsdir)
  callgrindfile = os.path.join(profiledir, 'callgrind.out.sniper')
  prof.writeCallgrind(file(callgrindfile, 'w'))

  gprof2dot_py = os.path.join(HOME, '..', 'gprof2dot.py')
  dotbasefile = os.path.join(profiledir, 'sim.profile')
  os.system('%s --format=callgrind --output=%s.dot %s' % (gprof2dot_py, dotbasefile, callgrindfile))
  if not distutils.spawn.find_executable('dot'):
    raise RuntimeError("Could not find `dot' executable, make sure graphviz is installed")
  os.system('dot -Tsvg %s.dot -o %s.svg' % (dotbasefile, dotbasefile))
  os.system('dot -Tpng %s.dot -o %s.png' % (dotbasefile, dotbasefile))
