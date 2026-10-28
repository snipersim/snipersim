"""
Regression fixture for the crash path (E9): the simulator, past its start-up,
takes a SIGSEGV of its own, and run-sniper must fail the run and print the
backtrace the simulator's handler wrote (simulator/exceptions.cc into
debug_backtrace.out, decoded by tools/gen_backtrace.py).  Nothing tested it.
"""
import os
import signal
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'scripts'))

import sim


class Crash:
  def hook_roi_begin(self):
    print('[CRASH] raising SIGSEGV in the simulator')
    sys.stdout.flush()
    os.kill(os.getpid(), signal.SIGSEGV)


sim.util.register(Crash())
