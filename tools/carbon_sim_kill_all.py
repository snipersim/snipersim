#!/usr/bin/env python

"""
This is the client process that kills n simulators
"""

import socket
import sys
import re

def kill_sim(host):
    port = 1999
    size = 1024
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host,port))
    s.send('c')
    data = s.recv(size)
    s.close()
    print 'Received:', data

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

# actual program
config_filename = "carbon_sim.cfg"
simulator_count = int(sys.argv[1])
process_list = load_process_list_from_file(config_filename)

for i in range(simulator_count):
    kill_sim(process_list[i])

