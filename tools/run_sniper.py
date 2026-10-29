import sys, os, re, time, subprocess, threading, tempfile, sniper_lib


# Which build to run.
#
# A build directory is self-contained -- its own config/sniper.py and its own
# lib/sniper -- so several configurations can exist side by side.  Lives here
# rather than in run-sniper because record-trace has to reach the same answer:
# run-sniper launches it as a subprocess, and a recorder from one build feeding
# a simulator from another is the kind of mismatch that shows up as a corrupt
# trace much later.
#
# Resolution order:
#   explicit (--build-dir)
#   $SNIPER_BUILD_DIR           for a shell that has picked one, and how
#                               run-sniper tells record-trace what it chose
#   $SNIPER_ROOT/build/default  the conventional symlink
def resolve_build_dir(home, explicit = None):
  candidates = []
  if explicit:
    candidates.append(('--build-dir', explicit))
  if os.getenv('SNIPER_BUILD_DIR'):
    candidates.append(('$SNIPER_BUILD_DIR', os.getenv('SNIPER_BUILD_DIR')))
  candidates.append(('build/default', os.path.join(home, 'build', 'default')))

  for origin, path in candidates:
    if os.path.isfile(os.path.join(path, 'config', 'sniper.py')):
      return os.path.abspath(path)
    if origin in ('--build-dir', '$SNIPER_BUILD_DIR'):
      print('Error: %s is %s, which has no config/sniper.py' % (origin, path), file=sys.stderr)
      print('       Configure and build it first.', file=sys.stderr)
      sys.exit(-1)

  print('Error: no build found.', file=sys.stderr)
  print('       Build one with `make`, or point --build-dir / $SNIPER_BUILD_DIR', file=sys.stderr)
  print('       at an existing build directory.', file=sys.stderr)
  sys.exit(-1)


def ensure_mpich(home, jobs = None):
  """The MPICH that deps/sniper-deps --mpi builds into <home>/mpich: its
  mpiexec, fetched on demand when it is not there (linked from the shared
  dependency cache, or built into it once), or None if that fails.  The MPI
  cases skipped, and run-sniper --mpi fell back to an mpiexec that is not
  there, until someone ran the fetch by hand; the other dependencies a run
  needs on demand are fetched the same way (tools/riscv/ime-check.sh)."""
  mpiexec = os.path.join(home, 'mpich', 'bin', 'mpiexec')
  if os.path.isfile(mpiexec):
    return mpiexec
  cmd = [ sys.executable, os.path.join(home, 'deps', 'sniper-deps'), '--mpi' ]
  if jobs:
    cmd += [ '--jobs', str(jobs) ]
  cmd += [ 'fetch' ]
  print('[SNIPER] No MPI in %s; fetching it: %s' % (home, ' '.join(cmd)), file = sys.stderr)
  sys.stderr.flush()
  try:
    # its progress to stderr: stdout may be a caller's parsed output
    rc = subprocess.call(cmd, stdout = sys.stderr.fileno())
  except OSError as e:
    print('[SNIPER] Could not run deps/sniper-deps: %s' % e, file = sys.stderr)
    return None
  if rc != 0 or not os.path.isfile(mpiexec):
    print('[SNIPER] Fetching MPI failed (exit %d)' % rc, file = sys.stderr)
    return None
  return mpiexec


def va2pa_valid():
  """Can this process read physical frame numbers from /proc/self/pagemap?

  Since Linux 4.0 only with CAP_SYS_ADMIN (bit 21 of CapEff); before, anyone
  could.  run-sniper and record-trace each had a shell one-liner for this:
  one tested the exit status of an echo, which is always 0, so it always said
  no; the other compared bytes with a str, which raises in Python 3.  Both
  also shifted by 20, CAP_SYS_RAWIO."""
  try:
    if int(os.uname().release.split('.')[0]) < 4:
      return True
    with open('/proc/self/status') as f:
      for line in f:
        if line.startswith('CapEff:'):
          return bool((int(line.split()[1], 16) >> 21) & 1)
  except (OSError, ValueError):
    pass
  return False


def decode_wait_status(rc, tag = 'SNIPER'):
  """Turn a raw os.wait4() status into an exit code, explaining signals.

  Lives here because both run-sniper and record-trace collect a status and
  both must exit with it.  record-trace collected it and then fell off the
  end of the file, so a recorder that failed to load its Pin tool -- no
  trace written -- still exited 0."""
  if os.WIFSIGNALED(rc):
    signum = os.WTERMSIG(rc)
    signal_names = {
      1: 'SIGHUP', 2: 'SIGINT', 3: 'SIGQUIT', 4: 'SIGILL',
      6: 'SIGABRT', 8: 'SIGFPE', 9: 'SIGKILL', 11: 'SIGSEGV',
      13: 'SIGPIPE', 14: 'SIGALRM', 15: 'SIGTERM'
    }
    signame = signal_names.get(signum, 'signal %d' % signum)
    print('[%s] ERROR: Process killed by %s (signal %d)' % (tag, signame, signum))
    if signum == 9:
      print('[%s] SIGKILL often indicates Out-Of-Memory (OOM) killer terminated the process.' % tag)
      print('[%s] Check: dmesg | grep -i "killed process" or /var/log/kern.log' % tag)
    elif signum == 11:
      print('[%s] SIGSEGV indicates a segmentation fault (memory access error).' % tag)
    elif signum == 6:
      print('[%s] SIGABRT indicates the process aborted (assertion failure or abort() call).' % tag)
    return 128 + signum  # Standard convention for signal-killed processes
  elif os.WIFEXITED(rc):
    code = os.WEXITSTATUS(rc)
    # The recorder runs under bash -c, which reports a child killed by a
    # signal as exit status 128 + the signal: name it, since a recorder that
    # dies silently otherwise only shows as streams ending early.
    if code > 128 and code < 128 + 65:
      print('[%s] ERROR: exited with status %d: the process it ran was killed by signal %d' % (tag, code, code - 128))
    elif code != 0:
      print('[%s] exited with status %d' % (tag, code))
    return code
  else:
    print('[%s] WARNING: Unknown process termination status: %d' % (tag, rc))
    return rc >> 8


def load_build_config(build_dir):
  """Read a build directory's config/sniper.py."""
  configfile = os.path.join(build_dir, 'config', 'sniper.py')
  config = {}
  exec(compile(open(configfile, "rb").read(), configfile, 'exec'), {}, config)
  return configfile, config

# The run stream.
#
# Every process in a run -- Sniper, the recorder, the application or the
# userspace kernel under it -- writes to one merged stream, and the merge is
# made here, line by line.  A Tee reads a pipe, and for each complete line
# writes it once (so lines from different processes never split each other)
# both to the terminal and to a file in the output directory.
#
# Two layers.  An application's Tee prefixes its process tag: "[app0] ", or
# "[kernel] " for the userspace MimicOS kernel.  run-sniper's own Tee, the
# outer one that everything reaches, stamps each line with the wall-clock
# time of arrival and tags whatever carries no process tag yet as [sniper]:
#
#   2026-09-17T02:14:33.123456 [kernel] [MimicOS] kernel 1: quantum_expired ...
#   2026-09-17T02:14:33.123512 [sniper] [TRACE:1] -- DONE --
#
# The stamped stream is what sim.stdout holds; tools/lnav parses it.
# Producers keep stdout line-buffered (see include/line_out.h) so arrival
# order is emission order.

TAGGED = re.compile(rb'^\[(?:app\d+|kernel|rank\d+)\] ')

class Tee:
  def __init__(self, filename, prefix = '', stamp = False, terminal = True):
    self.prefix = prefix.encode()
    self.stamp = stamp
    self.terminal = terminal        # False: file only (the screen belongs to sniper-live)
    self.file = open(filename, 'wb') if filename else None
    self.out_fd = os.dup(sys.stdout.fileno())    # where lines go: the stdout we were given
    self.r, self.w = os.pipe()
    self.thread = threading.Thread(target = self.__pump)
    self.thread.daemon = True
    self.thread.start()

  def fileno(self):
    return self.w

  def __emit(self, line):
    if line.endswith(b'\r'):
      line = line[:-1]
    line = self.prefix + line
    if self.stamp:
      if not TAGGED.match(line):
        line = b'[sniper] ' + line
      now = time.time()
      line = (time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(now)).encode()
              + b'.%06d ' % int((now % 1) * 1000000) + line)
    line += b'\n'
    if self.terminal:
      os.write(self.out_fd, line)
    if self.file:
      self.file.write(line)
      self.file.flush()

  def __pump(self):
    buf = b''
    while True:
      try:
        chunk = os.read(self.r, 65536)
      except OSError:
        break
      if not chunk:
        break
      buf += chunk
      while b'\n' in buf:
        line, buf = buf.split(b'\n', 1)
        self.__emit(line)
    if buf:
      self.__emit(buf)
    os.close(self.r)
    if self.file:
      self.file.close()

  def redirect(self):
    """Send this process's stdout and stderr through the tee."""
    sys.stdout.flush(); sys.stderr.flush()
    os.dup2(self.w, sys.stdout.fileno())
    os.dup2(self.w, sys.stderr.fileno())

  def close(self):
    """Restore stdout/stderr, then drain: every line written so far is out."""
    sys.stdout.flush(); sys.stderr.flush()
    os.dup2(self.out_fd, sys.stdout.fileno())
    os.dup2(self.out_fd, sys.stderr.fileno())
    os.close(self.w)
    self.thread.join(10)
    os.close(self.out_fd)

def __run_program_redirect(app_id, program_func, program_arg, outputdir, run_id = 0, tag = None, status = None):
  tee = Tee(os.path.join(outputdir, 'benchmark-app%d-run%d.log' % (app_id, run_id)), '[%s] ' % (tag or 'app%d' % app_id))
  tee.redirect()
  rc = 0
  try:
    rc = program_func(program_arg)
  finally:
    tee.close()
  # The function's status goes back through shared memory, not this
  # process's exit code: an exit code is 0..255, and a program that exited
  # with 255 could not be told from one killed by a signal.
  if status is not None and isinstance(rc, int):
    status.value = rc

def run_program_redirect(app_id, program_func, program_arg, outputdir, run_id = 0, tag = None):
  """Run program_func(program_arg) in a child process with its output tagged
  and logged; return what the function returned (0 when it returned nothing;
  for a subprocess's status, negative when the program was killed by a
  signal), or None when the child itself failed."""
  import multiprocessing # module does not exist in Python <= 2.5, import only when needed
  status = multiprocessing.Value('l', 0)
  proc = multiprocessing.Process(target = __run_program_redirect, args = (app_id, program_func, program_arg, outputdir, run_id, tag, status))
  proc.start()
  proc.join()
  if proc.exitcode != 0:
    return None
  return status.value

def run_program_repeat(app_id, program_func, program_arg, outputdir):
  global running
  run_id = 0
  while running:
    print('[RUN-SNIPER] Starting application', app_id)
    run_program_redirect(app_id, program_func, program_arg, outputdir, run_id)
    print('[RUN-SNIPER] Application', app_id, 'done')
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

# Determine libstdc++.so used by default by pin_sim.so (or sniper, if we aren't compiling pin_sim.so) using ldd
# Should take into account the current LD_LIBRARY_PATH
def get_cxx_inuse(sim_root, clear_ldlibpath = False):
  pin_sim = None
  for binary in ['%s/lib/pin_sim.so' % sim_root, '%s/lib/sniper' % sim_root]:
    if os.path.isfile(binary):
      pin_sim = binary
  if not pin_sim:
    return None
  try:
    ldd_out_name = tempfile.NamedTemporaryFile(delete = False).name
    ldlpsave = None
    if clear_ldlibpath:
      ldlpsave = os.environ.get('LD_LIBRARY_PATH', None)
      if ldlpsave:
        del os.environ['LD_LIBRARY_PATH']
    os.system('ldd %s > %s 2> /dev/null' % (pin_sim, ldd_out_name))
    if ldlpsave:
      os.environ['LD_LIBRARY_PATH'] = ldlpsave
    ldd_out = open(ldd_out_name).read()
    os.unlink(ldd_out_name)
    libcxx_path = os.path.dirname([ line.split()[2] for line in ldd_out.split('\n') if 'libstdc++.so.6' in line ][0])
  except Exception as e:
    print(repr(e), file=sys.stderr)
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
    except Exception as e:
      print(repr(e), file=sys.stderr)
      return 0
  else:
    return 0

def get_cxx_override(sim_root, pin_home, arch):
  # Find which libstdc++.so is newer: either the system default one (with or without the LD_LIBRARY_PATH), or the Pin one
  cxx_versions = [get_cxx_inuse(sim_root), get_cxx_inuse(sim_root, clear_ldlibpath = True), '%s/%s/runtime/cpplibs' % (pin_home, arch)]
  if 'BENCHMARKS_ROOT' in os.environ:
    cxx_versions.append('%s/libs' % os.environ['BENCHMARKS_ROOT'])
  cxx_versions = [x for x in cxx_versions if x!=None]
  cxx_override = sorted([(get_cxx_version(x),x) for x in cxx_versions], key=lambda x:x[0])[-1][1]
  return cxx_override

# LD_LIBRARY_PATH setup
#
# There are many different versions of LD_LIBRARY_PATH to consider:
# - the initial LD_LIBRARY_PATH which will affect Python when running this script
# - the application being simulated (PIN_APP_LD_LIBRARY_PATH):
#   SNIPER_APP_LD_LIBRARY_PATH, defaults to original LD_LIBRARY_PATH
# - the Sniper pintool or standalone executable and Pin itself (PIN_VM_LD_LIBRARY_PATH):
#   Pin runtime libraries, system libstdc++ (depending on version),
#   can be extended by setting SNIPER_SIM_LD_LIBRARY_PATH
# - scripts being run inside the simulator (SNIPER_SCRIPT_LD_LIBRARY_PATH): original LD_LIBRARY_PATH
#   (e.g. mcpat when running powertrace.py)

def setup_env(sim_root, pin_home, arch, standalone = False, xed_home = None, torch_home = None):

  env = dict(os.environ)
  ld_library_path_orig = env.get('LD_LIBRARY_PATH', '')

  # Construct Sniper/Pintool LD_LIBRARY_PATH
  ld_library_path = []
  cxx_override = get_cxx_override(sim_root, pin_home, arch)
  ld_library_path.append(cxx_override)
  if not standalone:
    ld_library_path.append('%s/%s/runtime/cpplibs' % (pin_home, arch))
    ld_library_path.append('%s/%s/runtime' % (pin_home, arch))
    # Not the kit's extras/xed-<arch>/lib.  Pin starts the application with
    # LD_LIBRARY_PATH set to this path (PIN_VM_LD_LIBRARY_PATH; the
    # application's own, PIN_APP_LD_LIBRARY_PATH, is restored only after
    # injection), so a program that itself links libxed.so -- the simulator
    # run as an application, test/sniper-in-sniper -- resolved it to the
    # kit's copy, which is built on Pin's C runtime, and died at exec with
    # "error while loading shared libraries: libpincrt.so".  The recorder tool
    # finds its XED through SDE; the in-Pin simulator that needed this entry
    # is gone.
  else:
    if xed_home:
      ld_library_path.append('%s/lib' % (xed_home,))
  if torch_home:
    ld_library_path.append('%s/lib' % (torch_home, ))
  if 'SNIPER_SIM_LD_LIBRARY_PATH' in os.environ:
    ld_library_path.append(os.environ['SNIPER_SIM_LD_LIBRARY_PATH'])
  env['LD_LIBRARY_PATH'] = ':'.join(ld_library_path)
  env['PIN_LD_RESTORE_REQUIRED'] = '1'
  env['PIN_VM_LD_LIBRARY_PATH'] = env['LD_LIBRARY_PATH'] # Pin VM and Pintool (Sniper) use LD_LIBRARY_PATH as modified above

  # Application LD_LIBRARY_PATH
  if 'SNIPER_APP_LD_LIBRARY_PATH' in env:
    env['PIN_APP_LD_LIBRARY_PATH'] = env['SNIPER_APP_LD_LIBRARY_PATH'] # Application uses explicit LD_LIBRARY_PATH
    del env['SNIPER_APP_LD_LIBRARY_PATH']
  else:
    env['PIN_APP_LD_LIBRARY_PATH'] = ld_library_path_orig  # Application uses original LD_LIBRARY_PATH

  # Scripts LD_LIBRARY_PATH
  env['SNIPER_SCRIPT_LD_LIBRARY_PATH'] = ld_library_path_orig  # Scripts running inside Sniper use original LD_LIBRARY_PATH

  # Other environment variables
  if 'SNIPER_APP_LD_PRELOAD' in env:
    env['PIN_APP_LD_PRELOAD'] = env['SNIPER_APP_LD_PRELOAD']
    del env['SNIPER_APP_LD_PRELOAD']
  elif 'LD_PRELOAD' in env:
    env['PIN_APP_LD_PRELOAD'] = env['LD_PRELOAD']
  env['LD_PRELOAD'] = ''
  env['PYTHONPATH'] = '%s/scripts:%s' % (sim_root, os.getenv('PYTHONPATH') or '')
  env['SNIPER_ROOT'] = sim_root
  env['GRAPHITE_ROOT'] = sim_root

  return env
