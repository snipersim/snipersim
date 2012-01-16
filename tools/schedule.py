#!/usr/bin/env python

# This takes a list of machines and a list of jobs and greedily runs
# them on the machines as they become available.

import os
import time
import shutil
import subprocess

import spawn

# Job:
#  a simple struct to hold data for a job
#  different subclasses of Job specialize for different types of commands
class Job:
    def __init__(self, num_machines, command):
        self.num_machines = num_machines
        self.command = command

    def spawn(self, wd):
        pass

    def poll(self):
        pass

    def wait(self):
        pass

# LocalJob:
#  just run something locally but consume lots of machines to do so
class LocalJob(Job):
    def __init__(self, num_machines, command):
        Job.__init__(self, num_machines, command)

    def spawn(self):
        self.pid = subprocess.Popen(self.command, shell=True)

    def poll(self):
        return self.pid.poll()

    def wait(self):
        return self.pid.wait()

# SpawnJob:
#  a job that should be run across several machines using the spawn
#  infrastructure
class SpawnJob(Job):
    def __init__(self, num_machines, command):
        Job.__init__(self, num_machines, command)

    def spawn(self):
        self.pid = spawn.spawn_job(self.machines, self.command)

    def poll(self):
        return spawn.poll_job(self.pid)

    def wait(self):
        return spawn.wait_job(self.pid)

# MakeJob:
#  a job built around the make system
class MakeJob(SpawnJob):

    def __init__(self, num_machines, command, results_dir="./output_files", sub_dir="", sim_flags=""):
        SpawnJob.__init__(self, num_machines, command)
        self.results_dir = results_dir
        self.sub_dir = "%s/%s" % (results_dir, sub_dir)
        self.sim_flags = sim_flags

    def make_pin_command(self):
        self.sim_flags += " -c %s/carbon_sim.cfg --general/output_dir=\"%s\" --general/num_processes=%d" % (self.results_dir, self.sub_dir, len(self.machines))
        for i in range(0,len(self.machines)):
            self.sim_flags += " --process_map/process%i=\"%s\"" % (i, self.machines[i])

        PIN_PATH = "/afs/csail/group/carbon/tools/pin/current/ia32/bin/pinbin"
        PIN_LIB = "%s/lib/pin_sim" % spawn.get_sim_root()

        self.command = "%s -mt -t %s %s -- %s" % (PIN_PATH, PIN_LIB, self.sim_flags, self.command)

        self.command += " >& %s/output" % self.sub_dir

        print self.command

    def spawn(self):

        # make output dir
        try:
            os.makedirs(self.sub_dir)
        except OSError:
            pass

        # format command
        self.make_pin_command()

        cmdfile = open("%s/command" % self.sub_dir,'w')
        cmdfile.write(self.command)
        cmdfile.close()
        
        SpawnJob.spawn(self)

# schedule - run a list of jobs across some machines
def schedule(machine_list, jobs):

    # initialization
    
    available = machine_list
    running = []

    # helpers
    def get_job(machine_list, jobs):
        for i in range(0,len(jobs)):
            if jobs[i].num_machines <= len(available):
                j = jobs[i]
                del jobs[i]
                j.machines = available[0:j.num_machines]
                del available[0:j.num_machines]
                del j.num_machines
                return j
        return

    # main loop
    while True:

        # spawn another job, if available
        job = get_job(available, jobs)

        if job != None:
            job.spawn()
            running.append(job)

        if len(jobs) == 0:
            break

        # check active jobs
        terminated = []
        for i in range(0,len(running)):
            status = running[i].poll()
            if status != None:
                available.extend(running[i].machines)
                terminated.append(i)

        terminated.reverse()
        for i in terminated:
            del running[i]
                
        time.sleep(0.1)

    # wait on remaining
    for j in running:
        job.wait()
