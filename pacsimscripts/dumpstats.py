import os, sys

##!/usr/bin/env python2
# class to hold representative stats

import argparse
from .pacsim_dumpstats import PerformStats
# dumpstats for Detailed Running
class DumpStats:

  def __init__(self, config):
    self.config = config;
    sniper_lib_path = "%(sniper_root)s/tools" % self.config
    if ( sniper_lib_path not in sys.path):
      # so that we can import sniper_lib
      sys.path.insert(0, "%(sniper_root)s/tools" % self.config)
    if "only-full-time" in self.config:
      self.only_full_time = config["only-full-time"]
    else:
      self.only_full_time = False
    self.stats_per_region = []
    self.region_count = 0
    self.total_time = 0
    self.populate_cluster_stats()
    return
  def populate_cluster_stats(self):
    import sniper_lib
    import sniper_stats
    jobid = 0
    resultsdir = "%(output_sniper_validate_dir)s/"%self.config
    stats = sniper_stats.SniperStats( resultsdir = resultsdir, jobid = jobid )
#    stats = sniper_lib.get_results( resultsdir=resultsdir,jobid=jobid)
    snapshots = stats.get_snapshots()

    markers = [snapshots[0]]
    print("snapshots",snapshots)
    for elem in snapshots:
        if "marker" in elem:
            tmps = elem.split('-')
            if(tmps[2]=='0'):
                markers.append(elem)
#            print(elem)
    markers.append(snapshots[-1])
    print(self.only_full_time)
    if not self.only_full_time:
        for i in range(0,len(markers)-1):
            m1 = markers[i]
            m2 = markers[i+1]
#            print(  m1,m2)
            stats = PerformStats(self.config,(m1,m2), dvfs=[],validate=True,roi = True)
            self.stats_per_region.append(stats)

        self.region_count = len(self.stats_per_region)

    m1 = markers[0]
    m2 = markers[-1]
    print(m1,m2)
    stats = PerformStats(self.config,(m1,m2), dvfs=[],validate=True,roi=True)
    self.walltime = max(stats.walltime_per_core)/1e6
    self.total_time = stats.time0/1e15
    print(stats.l2misscounts)
    print("inscount",stats.instcounts)
    print(stats.l2counts)
    #self.l3missrate = 1000.0*stats.l2misscounts/ stats.instcounts
    self.l2misscounts = stats.l2misscounts
    self.instcounts = stats.instcounts
    return
  def print_l3_missrate(self):
    return self.instcounts
#    return self.l2misscounts

  def print_full_time(self):
    return self.total_time
  def print_wall_time(self):
    return self.walltime
  def print_overall_runtime(self):
    total_time = 0.0

    time_of_each_region = []
    for i in range(self.region_count):
      # get stats
      stats = self.stats_per_region[i]
      # aggregate total simulated runtime
    #  print( stats.time0 )
      time_of_each_region.append( stats.time0/1e15)

#    print "[DUMPSTATS] Overall Simulated Runtime: ", total_time/1e15, ' seconds'
    return time_of_each_region
  def print_walltime(self):
    time_of_each_region = []
    total_time = 0.0
    for i in range(self.region_count):
      # get stats
      stats = self.stats_per_region[i]
      runtime1 = float(max( stats.walltime_per_core))

      total_time += runtime1/1e6

#    print "[DUMPSTATS] Overall Simulated Runtime: ", total_time/1e15, ' seconds'
    return total_time
def get_args():
    parser = argparse.ArgumentParser(description="Analysing data for detail mode")

    parser.add_argument("--sniper-root", type=str, default=os.environ.get("SNIPER_ROOT"), help="root of sniper")
    parser.add_argument("--dir", type=str, default=None, help="output of sniper")
    parser.add_argument("--segs" , action="store_true", default=False,help="to print time of between each marker")
    args = parser.parse_args()
#    print(args.only_full_time)
    config = dict()
    config["sniper_root"] = args.sniper_root
    config["only-full-time"] = not args.segs
    if args.dir is None:
        print(" Sniper output dir should be assigned"  )
        exit(1)
    config["output_sniper_dir"] = args.dir

    config[ "output_sniper_validate_dir"] = args.dir
    return config
def get_full_version_time( output_sniper_dir, sniper_root_dir ):
    config = dict()

    config[ "output_sniper_validate_dir"] = output_sniper_dir
    config["sniper_root"] = sniper_root_dir
    hyper_dump_stat = DumpStats( config )

    total_time_for_each_region = hyper_dump_stat.print_overall_runtime()
    return  total_time_for_each_region

def get_full_time( output_sniper_dir, sniper_root_dir ):
    config = dict()

    config[ "output_sniper_validate_dir"] = output_sniper_dir
    config["sniper_root"] = sniper_root_dir
    config["only-full-time"] = True
    hyper_dump_stat = DumpStats( config )

    total_time_for_each_region = hyper_dump_stat.print_full_time()
    walltime = hyper_dump_stat.print_wall_time()
    
    return  total_time_for_each_region,walltime,hyper_dump_stat.print_l3_missrate()

if __name__ == "__main__":
    config = get_args(  )
#    print(config)
    dump_stat = DumpStats( config )

    total_time_for_each_region = dump_stat.print_full_time()
    wall_time = dump_stat.print_wall_time()
    print( total_time_for_each_region)
    print( "Wall Time:", wall_time)
 # def populate_cluster_stats(self):




 # def populate_cluster_stats(self):

