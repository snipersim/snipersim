import sys, os, time, subprocess, threading, sniper_lib

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
