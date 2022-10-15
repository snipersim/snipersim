#!/usr/bin/env python2

import sys, os, tempfile

pid = int(sys.argv[1])
mytid = int(sys.argv[2])

# FIXME: optionally attach GDB to each thread since GDB won't find Pintool threads by itself
#for tid in os.listdir('/proc/%d/task' % pid):
#  tid = int(tid)
#  print >> sys.stderr, "Starting screen with GDB for thread %d; resume using screen -r gdb-%d" % (tid, tid)
#  os.system('screen -d -m -S gdb-%d -- gdb $SNIPER_ROOT/pin_kit/intel64/bin/pinbin %d' % (tid, tid))

cmdfile = '/tmp/gdbcmd-%d' % mytid
open(cmdfile, 'w').write('''\
# Continue execution past the sleep in pinboost_debugme() and into the real exception
continue
# Show a backtrace at the exception
backtrace
''')

print >> sys.stderr
print >> sys.stderr, "[PINBOOST] Starting screen session with GDB attached to thread %d; resume using" % mytid
print >> sys.stderr, "# screen -r gdb-%d" % mytid

os.system('screen -d -m -S gdb-%d -- gdb -command=%s $SNIPER_ROOT/pin_kit/intel64/bin/pinbin %d' % (mytid, cmdfile, mytid))
