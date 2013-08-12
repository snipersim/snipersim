import sys, os, time, subprocess, threading, tempfile, sniper_lib

def __run_program_redirect(app_id, program_func, program_arg, outputdir, run_id = 0):
  out = file(os.path.join(outputdir, 'benchmark-app%d-run%d.log' % (app_id, run_id)), 'w', 0) # Open unbuffered to maintain stdout/stderr interleaving
  os.dup2(out.fileno(), sys.stdout.fileno())
  #os.dup2(out.fileno(), sys.stderr.fileno())
  program_func(program_arg)

def run_program_redirect(app_id, program_func, program_arg, outputdir, run_id = 0):
  import multiprocessing # module does not exist in Python <= 2.5, import only when needed
  proc = multiprocessing.Process(target = __run_program_redirect, args = (app_id, program_func, program_arg, outputdir, run_id))
  proc.start()
  proc.join()

def run_program_repeat(app_id, program_func, program_arg, outputdir):
  global running
  run_id = 0
  while running:
    print '[RUN-SNIPER] Starting application', app_id
    run_program_redirect(app_id, program_func, program_arg, outputdir, run_id)
    print '[RUN-SNIPER] Application', app_id, 'done'
    time.sleep(1)
    run_id += 1

def run_multi(snipercmd, applications, repeat = False, outputdir = '.'):
  global running
  running = True
  p_sniper = subprocess.Popen([ 'bash', '-c', snipercmd ])

  threads = []
  for app in applications:
    t = threading.Thread(target = repeat and run_program_repeat or run_program_redirect,
                         args = (app['app_id'], app['func'], app['args'], outputdir))
    threads.append(t)

  for t in threads:
    t.start()
  p_sniper.wait()
  running = False # Simulator has ended, signal the benchmarks to stop restarting

  time.sleep(2)
  # Clean up benchmarks
  sniper_lib.kill_children()
  for t in threads:
    t.join()

  return p_sniper.returncode

# Determine libstdc++.so used by default by pin_sim.so using ldd
# Should take into account the current LD_LIBRARY_PATH
def get_cxx_inuse(sim_root):
  pin_sim = '%s/lib/pin_sim.so' % sim_root
  try:
    ldd_out_name = tempfile.NamedTemporaryFile(delete = False).name
    os.system('ldd %s > %s 2> /dev/null' % (pin_sim, ldd_out_name))
    ldd_out = open(ldd_out_name).read()
    os.unlink(ldd_out_name)
    libcxx_path = os.path.dirname([ line.split()[2] for line in ldd_out.split('\n') if 'libstdc++.so.6' in line ][0])
  except Exception, e:
    print >> sys.stderr, `e`
    return None
  return libcxx_path

# Find libstdc++.so version number in a given path
def get_cxx_version(path):
  filename = os.path.join(path, 'libstdc++.so.6')
  if os.path.exists(filename):
    realname = os.path.realpath(filename)
    try:
      version = int(realname.split('.')[-1])
      return version
    except Exception, e:
      print >> sys.stderr, `e`
      return 0
  else:
    return 0

def get_cxx_override(sim_root, pin_home, arch):
  # Find which libstdc++.so is newer: either the system default one, or the Pin one
  cxx_versions = [get_cxx_inuse(sim_root), '%s/%s/runtime/cpplibs' % (pin_home, arch)]
  if 'BENCHMARKS_ROOT' in os.environ:
    cxx_versions.append('%s/libs' % os.environ['BENCHMARKS_ROOT'])
  cxx_override = sorted(map(lambda x:(get_cxx_version(x),x), cxx_versions), key=lambda x:x[0])[-1][1]
  return cxx_override
