#!/usr/bin/env python2

import threading, subprocess, sys, os, tempfile

def execute_gdb(cmd, env, pin_home, arch, quiet = False, wait = False, quit = False):
  if wait and quit:
    raise ValueError('Cannot call execute_gdb() with both wait and false == True')

  if not quiet:
    print 'Running', cmd
    sys.stdout.flush()
    sys.stderr.flush()

  p_graphite = subprocess.Popen([ 'bash', '-c', cmd ], bufsize = 1, stdout = subprocess.PIPE, env = env)
  g_pid = 0
  g_symbols = []
  while True:
    line = p_graphite.stdout.readline()
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
      print line,
  threading.Thread(target = output_graphite).start()

  fh, fn = tempfile.mkstemp()
  f = open(fn, 'w')
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

  rc = os.system('gdb -quiet -command=%s %s' % (fn, '%(pin_home)s/%(arch)s/bin/pinbin' % locals()))
  rc >>= 8

  return rc
