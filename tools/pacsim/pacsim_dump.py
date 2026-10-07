#!/usr/bin/env python3
import os, sys

# class to hold representative stats
import json
import argparse
from .pacsim_dumpstats import PerformStats
# dumpstats for Detailed Running
#from tqdm import tqdm
#from multiprocessing import Pool, cpu_count
from multiprocessing import  cpu_count
from multiprocessing.pool import ThreadPool as Pool
FastForwardMode = 1
DetailMode = 0

clusters = dict()
class SegmentData:
    def __init__(self, m1,m2, record,config):
        self.config = config
        self.m1 = m1
        self.m2 = m2
        ##( record type, sim mode, pc)
        self.sim_mode = record[1]
        self.segmentType = record[0]
        self.pc = record[2]
        if( self.sim_mode == FastForwardMode ) :
            stats = PerformStats(self.config,(m1,m2), dvfs=[],roi = False)
        else:
            stats = PerformStats(self.config,(m1,m2), dvfs=[],roi = True)
            clusters[ self.pc ] = stats
        self.stats = stats
    def get_time(self):

        if self.sim_mode == FastForwardMode:
    #self.walltime = max(stats.walltime_per_core)/1e6
            stats_template = clusters[ self.pc ]
            stats = self.stats
#            spi = ( float(stats_template.time0)) / sum(stats_template.instruction_count_per_core)
#            time = spi * sum( stats.instruction_count_per_core )
            diff = float( sum( stats.instruction_count_per_core ) ) / float( sum(stats_template.instruction_count_per_core) )
            if diff <= 1.2:
                time = stats_template.time0 * diff
            else:
                time = stats_template.time0
#            time1=time/1e15
#            time2=stats_template.time0/1e15
#            if(time1 >= 0.01 or time2>=0.01):
#                print ("{:.2f}".format(time1) ,  "{:.2f}".format(time2) )

        else:
            stats = self.stats
            time = stats.time0
        return time

class PacSimDumpStats:

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
############init marker:
    snapshots = stats.get_snapshots()
    markers = [snapshots[0]]
    for elem in snapshots:
        if "marker" in elem:
            tmps = elem.split('-')
            if(tmps[2]=='2'):
                markers.append(elem)
    markers.append(snapshots[-1])
############init sim mode and segment type
    # Reachable only from toolset.py, which does not run in this tree (see the
    # header there).  The live reader is pacsim_dumpjson, which accepts both
    # pacsim_regions.json and the older intra_mtng.json; this one is left on the
    # older name rather than renamed to something nothing writes.
    with open( "%(output_sniper_validate_dir)s/mtng.json"%self.config ) as f:
        json_raw = f.read()
        record = json.loads(json_raw)['record']
    ##( record type, sim mode, pc)
    records = [[2,DetailMode,0]] + record
    segments = []

    progress = True
    bar = tqdm if progress else lambda iterable, total, desc: iterable
    def f(i):
        m1,m2,record = markers[i],markers[i+1],records[i]
        return SegmentData(m1,m2,record,self.config)
    print(cpu_count())
    pool = Pool(cpu_count())
    pool = Pool(1)
    data = list(range(len(markers)-1))
    segments = list(
                    bar(
                        pool.imap( f, data ),
                        total=len(markers)-1,
                        desc="get all segments"
                        )
                )
#    segments = []
#    for i in range(0,len(markers)-1):
#        m1 = markers[i]
#        m2 = markers[i+1]
#        record = records[i]
#        segments.append(SegmentData(m1,m2,record,self.config))
    total_time = 0
    for segment in segments:
        time = segment.get_time()
        total_time += time
    m1 = markers[0]
    m2 = markers[-1]
    stats = PerformStats(self.config,(m1,m2), dvfs=[],roi = False)
    self.walltime = max(stats.walltime_per_core)/1e6
    self.total_time = total_time/1e15
    return
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
    hyper_dump_stat = PacSimDumpStats( config )

    total_time_for_each_region = hyper_dump_stat.print_overall_runtime()
    return  total_time_for_each_region

def get_full_time( output_sniper_dir, sniper_root_dir ):
    config = dict()

    config[ "output_sniper_validate_dir"] = output_sniper_dir
    config[ "output_sniper_dir"] = output_sniper_dir
    config["sniper_root"] = sniper_root_dir
    config["only-full-time"] = True
    hyper_dump_stat = PacSimDumpStats( config )

    total_time_for_each_region = hyper_dump_stat.print_full_time()
    walltime = hyper_dump_stat.print_wall_time()

    return  total_time_for_each_region,walltime

if __name__ == "__main__":
    config = get_args(  )
#    print(config)
    dump_stat = PacSimDumpStats( config )

    total_time_for_each_region = dump_stat.print_full_time()
    wall_time = dump_stat.print_wall_time()
    print( total_time_for_each_region)
    print( "Wall Time:", wall_time)
 # def populate_cluster_stats(self):




 # def populate_cluster_stats(self):

