#!/usr/bin/env python

import sys
import os

sys.path.append("./tools/")

from schedule import *

# job info
machines = [
    "cagnode1",
    "cagnode2",
    "cagnode3",
    "cagnode4",
    "cagnode5",
    "cagnode6",
    "cagnode7",
    "cagnode9",
    "cagnode10",
    "cagnode11",
    "cagnode17",
    "cagnode18"
    ]

results_dir = "./results/george-fmm-6.8ocbw"

command = "./tests/benchmarks/fmm/fmm < ./tests/benchmarks/fmm/inputs/input.256"

sim_flags = "--perf_model/dram/offchip_bandwidth=6.8 --dram_dir/max_sharers=7"

jobs = [
    LocalJob(12, "echo Starting..."),
    MakeJob(4, command, results_dir, "1", sim_flags),
    MakeJob(4, command, results_dir, "2", sim_flags),
    MakeJob(4, command, results_dir, "3", sim_flags),
    MakeJob(4, command, results_dir, "4", sim_flags),
    MakeJob(4, command, results_dir, "5", sim_flags),
    MakeJob(4, command, results_dir, "6", sim_flags),
    MakeJob(4, command, results_dir, "7", sim_flags),
    MakeJob(4, command, results_dir, "8", sim_flags),
    MakeJob(4, command, results_dir, "9", sim_flags),
    LocalJob(12, "echo Finished."),
    ]

# uncomment this line to kill all the fmms
#jobs = [ SpawnJob(12, "killall -9 fmm") ]

# init
try:
    os.makedirs(results_dir)
except OSError:
    pass

shutil.copy("carbon_sim.cfg", results_dir)

# go!
schedule(machines, jobs)
