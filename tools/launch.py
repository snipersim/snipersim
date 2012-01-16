#!/usr/bin/python

import sys
import subprocess
import time
import signal
import os

start_proc = int(sys.argv[1])
num_procs = int(sys.argv[2])

path = ""
for p in sys.argv[3:len(sys.argv)]:
    path += p + " "

#print "Executing %d copies of program: %s" % (num_procs, path)

procs = {}

# start
for i in range(start_proc, start_proc + num_procs):
    print "[launch.py] Starting process: %d" % i
    env = {}
    env["CARBON_PROCESS_INDEX"] = str(i)
    env["LD_LIBRARY_PATH"] = "/afs/csail/group/carbon/tools/boost_1_38_0/stage/lib"
    procs[i] = subprocess.Popen(path, shell=True, env=env)

# wait
active = True
returnCode = None

while active:
    time.sleep(0.1)
    for i in range(start_proc, start_proc + num_procs):
        returnCode = procs[i].poll()
        if returnCode != None:
            active = False
            break

# kill
for i in range(start_proc,start_proc + num_procs):
   returnCode2 = procs[i].poll()
   if returnCode2 == None:
      if returnCode == 0:
         returnCode = procs[i].wait()
      else:
         os.kill(procs[i].pid, signal.SIGKILL)

# exit
if returnCode != None:
    print '[launch.py] Exited with return code: %d' % returnCode

sys.exit(returnCode)
