#!/usr/bin/env python

"""
This is the client process that distributes n simulators
"""

import socket
import sys
import re
import subprocess
import threading
import time
import select

class SimInteractor( threading.Thread ):
    def run( self ):
        while True:
            rd, wr, er =  select.select([sys.stdin],[],[],.25)
            if len(rd):
                data = sys.stdin.readline()
                sim_list[0].send(data)
            elif self.running == False:
                return

    def stop(self):
        self.running = False

    def __init__ (self):
        self.running = True
        threading.Thread.__init__(self)

def spawn_sim(host, id, path):
    port = 1999
    size = 1024
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host,port))
    s.send('%s%s,%s' % ('s',id,path))
    return s

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

# for spawning locally
def spawn_proc(command):
    env = {}
    env["CARBON_PROCESS_INDEX"] = str(0)
    env["LD_LIBRARY_PATH"] = "/afs/csail/group/carbon/tools/boost_1_38_0/stage/lib"
    proc = subprocess.Popen(command, shell=True, env=env)
    proc.wait()

# actual program
config_filename = "carbon_sim.cfg"
simulator_count = int(sys.argv[1])
command = ""
for i in range(2,len(sys.argv)):
    command += sys.argv[i] + " "

process_list = load_process_list_from_file(config_filename)

# determine if we will spawn locally or if we will distribute
if simulator_count > 1:
    sim_list = []
    # spawn the simulators
    for i in range(simulator_count):
        sim_list.append(spawn_sim(process_list[i],i,command))

    sim_interactor = SimInteractor()
    sim_interactor.start()

    # wait for them to finish
    for s in sim_list:
        try:
            while True:
                data = s.recv(1024)
                if len(data) == 0:
                    break
                sys.stdout.write(data)
        except:
            print "Exception occurred."
            s.close()

        #data = s.recv(3)
        #print 'Received: ', data

        ## now get the stdout/stderr
        #data = s.recv(1024)

        #split_data = data.split(",")
        #sout_size = split_data[0]
        #serr_size = split_data[1]

        ## chop off args to construct lhs
        #lhs = data[len(sout_size) + len(serr_size) + 2:]

        ## read what we haven't already
        #rhs = s.recv(int(sout_size) + int(serr_size) - len(lhs))

        ## combine the two outputs
        #stdout_and_stderr = "%s%s" % (lhs, rhs)

        ## extract stdout and stderr
        #stdout = stdout_and_stderr[0:int(sout_size)]
        #stderr = stdout_and_stderr[int(sout_size):]

        #print 'stdout: \n%s\n' % stdout
        #print 'stderr: \n%s\n' % stderr

        #s.close()


    sim_interactor.stop()
else:
    spawn_proc(command)

