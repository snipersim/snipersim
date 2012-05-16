#!/usr/bin/env python

"""
This is the client process that distributes n simulators
"""

import sys
import os
import re
import subprocess
import time
import signal

execfile('config/graphite.py')


# spawn_job:
#  start up a command across multiple machines
#  returns an object that can be passed to poll_job()
def spawn_job(machine_list, command, working_dir = os.getcwd()):

    procs = {}

    # spawn
    for i in range(0,len(machine_list)):

        exec_command = "export CARBON_PROCESS_INDEX=" + str(i) + "; " + \
                       command

        if (machine_list[i] != "localhost") and (machine_list[i] != r'127.0.0.1'):
            exec_command = exec_command.replace("\"","\\\"")
            exec_command = "ssh -x " + machine_list[i] + \
                           " \"cd " + working_dir + "; " + \
                           exec_command + "\""

        print "[spawn.py] Starting process: " + str(i) + " : " + exec_command
        procs[i] = subprocess.Popen(exec_command, shell=True)

    return procs

# poll_job:
#  check if a job has finished
#  returns the returnCode, or None
def poll_job(procs):

    # check status
    returnCode = None

    for i in range(0,len(procs)):
        returnCode = procs[i].poll()
        if returnCode != None:
            break

    # process still running
    if returnCode == None:
        return None

    # process terminated, so wait or kill remaining
    for i in range(0,len(procs)):
        returnCode2 = procs[i].poll()
        if returnCode2 == None:
            if returnCode == 0:
                returnCode = procs[i].wait()
            else:
                os.kill(procs[i].pid, signal.SIGKILL)

    print "[spawn.py] Exited with return code: %d" % returnCode

    return returnCode

# wait_job:
#  wait on a job to finish
def wait_job(procs):

    while True:
        ret = poll_job(procs)
        if ret != None:
            return ret
        time.sleep(0.5)

# helper functions

# read process list from a config file
def load_process_list_from_file(filename):
    process_list = []
    config = open(config_filename,"r").readlines()
    found_process_map = False
    for line in config:
        if found_process_map:
            if line == "\n":
                break;
            # extract the process from the line
            hostname = re.search("\"(.*)\"", line).group(1)
            process_list.append(hostname)

        if line == "[process_map]\n":
            found_process_map = True

    return process_list

# get sim root from environment variable, or use pwd
def get_sim_root():

    sim_root = os.environ.get('GRAPHITE_ROOT')

    if sim_root == None:
        pwd = os.environ.get('PWD')
        assert(pwd != None)
        print "[spawn.py] 'GRAPHITE_ROOT' undefined. Setting 'GRAPHITE_ROOT' to '" + pwd
        return pwd

    return sim_root

# main -- if this is used as standalone script
if __name__=="__main__":
    num_procs = int(sys.argv[1])
    config_filename = sys.argv[2]
    command = " ".join(sys.argv[3:])

    process_list = load_process_list_from_file(config_filename)

    j = spawn_job(process_list[0:num_procs],
                  command,
                  get_sim_root())

    sys.exit(wait_job(j))
