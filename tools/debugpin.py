#!/usr/bin/env python3

import threading, subprocess, sys, os, tempfile

def execute_gdb(cmd, env, pin_home, arch, quiet = False, wait = False, quit = False, pinbin = None, status = False):
  """Run cmd, a Pin command line with Pin's -pause_tool, and attach gdb to
  Pin once it pauses.  Returns gdb's exit status; with status=True, waits for
  cmd once gdb is done and returns its raw wait status instead (as
  os.waitpid gives it).  pinbin: the Pin binary gdb loads (default: Pin's
  kit layout under pin_home)."""
  if wait and quit:
    raise ValueError('Cannot call execute_gdb() with both wait and false == True')

  if not quiet:
    print('Running', cmd)
    sys.stdout.flush()
    sys.stderr.flush()

  p_graphite = subprocess.Popen([ 'bash', '-c', cmd ], bufsize = 1, stdout = subprocess.PIPE, env = env, text=True)
  g_pid = 0
  g_symbols = []
  seen = []
  while True:
    line = p_graphite.stdout.readline()
    seen.append(line)
    if not line:
      print(''.join(seen), end = '')                  # what it said instead
      # The pipe closed before Pin paused: no pause knob on the command
      # line, or Pin failed first.  This read the closed pipe for ever.
      print('[DEBUGPIN] The command ended before Pin paused for gdb', file=sys.stderr)
      _, st = os.waitpid(p_graphite.pid, 0)
      return st if status else 1
    if line.startswith('Pausing to attach to pid') or (line.startswith('Pausing for') and 'to attach to process' in line):
      g_pid = line.split()[-1]
    elif ('add-symbol-file' in line) or ('file' in line) or ('set' in line):
      g_symbols.append(line.rstrip())
    else:
      if g_pid and g_symbols:
        break

  def output_graphite():
    while True:
      line = p_graphite.stdout.readline()
      if not line: break
      print(line, end=' ')
  threading.Thread(target = output_graphite).start()

  # gdb's command file, removed once gdb is done with it (it was left in /tmp
  # by every --gdb run, and its descriptor left open)
  fh, fn = tempfile.mkstemp(prefix = 'sniper-gdb-', suffix = '.gdb')
  f = os.fdopen(fh, 'w')
  f.write('attach %s\n%s\n' % (g_pid, '\n'.join(g_symbols)))
  if not wait:
    f.write('continue\n')
  # Only quit GDB when we have not seen a signal
  if quit:
    f.write('if ($_siginfo)\n')
    f.write('else \n')
    f.write(' quit\n')
    f.write('end\n')
  f.close()

  if not pinbin:
    pinbin = '%(pin_home)s/%(arch)s/bin/pinbin' % locals()
  try:
    rc = os.system('gdb -quiet -command=%s %s' % (fn, pinbin))
  finally:
    try:
      os.unlink(fn)
    except OSError:
      pass
  rc >>= 8

  if status:
    _, st = os.waitpid(p_graphite.pid, 0)
    return st
  return rc
