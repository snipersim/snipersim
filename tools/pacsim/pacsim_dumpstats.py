#!/usr/bin/env python2
import os, sys

# class to hold representative stats
class PerformStats:
  def __init__(self,config,mtuple,dvfs=[],validate=False,roi =False):
    m1, m2 = mtuple
#    if not roi :
#        return
    from .. import sniper_lib
    self.config = config
    if (validate):
        # get perform stat from validate directory
        self.results = sniper_lib.get_results(resultsdir='%(output_sniper_validate_dir)s' % self.config , jobid=0, partial = [m1,m2])
    else:
        self.results = sniper_lib.get_results(resultsdir='%(output_sniper_dir)s' % self.config , jobid=0, partial = [m1,m2])

    self.ncores = int(self.results['config']['general/total_cores'])
    self.l2counts = sum(self.results['results']['L2.loads'] + self.results['results']['L2.stores'])
    self.l2misscounts = sum(self.results['results']['L2.load-misses'] + self.results['results']['L2.store-misses'])
    self.instcounts = sum( self.results['results']['core.instructions']  )
    print(self.instcounts)
    if 'barrier.global_time' in self.results['results']:
      self.time0 = self.results['results']['barrier.global_time'][0]
#      print(   self.results['results']['barrier.global_time'][0])
    else:
      self.time0 = self.results['results']['barrier.global_time_end'][0] - self.results['results']['barrier.global_time_begin'][0]  # duration
#      print(   self.results['results']['barrier.global_time_end'][0] ,  self.results['results']['barrier.global_time_begin'][0] )



    self.is_roi = roi;
    # if not in roi, time0 should be 0, since it is not simulated
    # TODO: This is not true. Why?
    # assert(self.is_roi or self.time0 == 0)

    if (self.is_roi):
      # representative region
      self.instruction_count_per_core = self.results['results']['performance_model.instruction_count']
      # self.instruction_count_per_core = self.results['results']['core.instructions']
      if (dvfs):
        self.cycle_count_per_core = [ self.time0 * float(dvfs[c]) * 1e6 / 1e15 for c in range(self.ncores)] # first convert cpufreq to Hz, then divide by femtosec
      else:
        self.cycle_count_per_core = [ self.time0 * self.results['results']['fs_to_cycles_cores'][c] for c in range(self.ncores)]

      self.ipc_per_core = [ins / (c or 1) for ins, c in 
              zip(self.instruction_count_per_core, self.cycle_count_per_core)]

      self.walltime_per_core = self.results['results']['time.walltime']
    else:
      # save some basic info of fastforward stats
      # walltime in microseconds
      self.walltime_per_core = self.results['results']['time.walltime']
      self.instruction_count_per_core = self.results['results']['core.instructions']
    #print( self.instruction_count_per_core )
    # sanity
    # assert( sum(self.instruction_count_per_core) > 0)
    return


# dumpstats for mtng
class DumpStatsMtng:

  def __init__(self, config):
    self.config = config;
    sniper_lib_path = "%(sniper_root)s/tools" % self.config
    if ( sniper_lib_path not in sys.path):
      # so that we can import sniper_lib
      sys.path.insert(0, "%(sniper_root)s/tools" % self.config)

    # key: cluster_id, value: PerformStats object
    self.cluster_stats_dict = {}
    self.cluster_labels = []
    self.cpufreq_per_region = []
    self.stats_per_region = []
    self.region_count = 0
    self.populate_cluster_stats()
    return


  def populate_cluster_stats(self):
    # get barrier region to cluster id map
    # region_labels[BARRIER_ID] = REGION_ID
    ''' The old method use t.labels and bbv map to associate atomic region id to cluster id.
    In the new method, we dumped out runtime region id to cluster id so we can use that directly
    with open('%(output_cluster_dir)s/t.labels' % self.config,'r') as f, open('%(output_bbv_dir)s/barrier_bbv.txt_thread_map' % self.config,'r') as f2:
      cnt_main_thread_regions = 0
      while (1):
        line = f2.readline()
        line = line.strip('\n').split(' ')
        if line[0] == '0':
          cnt_main_thread_regions += 1
        else:
          break
      print("Counted ", cnt_main_thread_regions , " regions from main thread.")
      for i in range(cnt_main_thread_regions):
        line = f.readline()
        if (line == ''):
          break
        # get label for each barrier region
        self.cluster_labels.append(int(line.split(' ')[0]))     
    self.region_count = len(self.cluster_labels)
    assert(self.region_count > 0)
    print (self.cluster_labels)
    '''
    
    with open('%(output_sniper_dir)s/sim.region_map' % self.config,'r') as f:
        for line in f:
            line = line.strip('\n').split(' ')
            region_id = int(line[0])
            cluster_id = int(line[1])
            self.cluster_labels.append(cluster_id)     
        self.region_count = len(self.cluster_labels)
        assert(self.region_count > 0)
        print (self.cluster_labels)
    # get DVFS profile
    '''
    with open('%(output_sniper_dir)s/sim.dvfs' % self.config,'r') as f:
        for line in f:
            line = line.strip(' \n').split(" ")
            # the first element is the region id. We do not need it now.
            line = [int(i) for i in line[1:]] 
            self.cpufreq_per_region.append(line)
    if (self.region_count < len(self.cpufreq_per_region)):
        print "[Warning] Found mismatch between dvfs regions and barrier regions (probably due to corner regions before/after ROI)"
        print "[Warning] dvfs regions: %d, barrier regions: %d" %(len(self.cpufreq_per_region), self.region_count)
        print "[Warning] Truncating dvfs regions"
        self.cpufreq_per_region = self.cpufreq_per_region[:self.region_count]
    # print(self.region_count ,  " " ,len(self.cpufreq_per_region) );
    # assert(self.region_count == len(self.cpufreq_per_region))
    '''

    seen = set();
    # Get all region snapshots, populate cluster stats dict
    for i in range(self.region_count):
      try:
        m1 = "marker-"+str(i)+"-0"
        m2 = "marker-"+str(i+1)+"-0"
        # stats = PerformStats(self.config,(m1,m2), dvfs=self.cpufreq_per_region[i]);
        cluster_id = self.cluster_labels[i]
        size_before = len(seen)
        seen.add(cluster_id)
        size_after = len(seen)
        is_roi = size_before < size_after
        stats = PerformStats(self.config,(m1,m2), dvfs=[],roi = is_roi); 
        if (is_roi):
          print ("ROI: ", i)
          # Allow overwriting of ROI, after resimulation
          # assert(cluster_id not in self.cluster_stats_dict)
          self.cluster_stats_dict[cluster_id] = stats
        # still save a copy of region stats that are fastforwarded, for MIPS (need walltime info)
        self.stats_per_region.append(stats)
      except Exception as e:
        # Some error inserting marker towards the end, discard the last region
        print (e)
        print ("[DUMPSTATS ERROR] Found unterminated markers at %s to %s, discard the rest" %(m1,m2))
        # raise e
        self.region_count = i
        break

    assert(len(self.cluster_stats_dict) > 0)
    return 

  def print_overall_ipc(self):
    # calculate Overall IPC
    total_ins = 0
    total_cycles = 0
    for i in range(self.region_count):
      # get stats
      cluster_id = self.cluster_labels[i]
      stats = self.cluster_stats_dict[cluster_id]
        
      # aggregate total cycles and total instructions 
      total_cycles += sum(stats.cycle_count_per_core)
      total_ins += sum(stats.instruction_count_per_core)

    print ("[DUMPSTATS] Overall IPC: ", total_ins / total_cycles)
    return total_ins / total_cycles


  def plot_ipc(self):
    # Plotting IPC
    time = 0.0
    with open("ipc_dump.tsv","w") as f:
      for i in range(self.region_count):
        cluster_id = self.cluster_labels[i]
        stats = self.cluster_stats_dict[cluster_id]
        f.write(str(time))
        f.write('\t')
        for ipc in stats.ipc_per_core:
            f.write(str(ipc))
            f.write('\t')
        time += stats.time0
        f.write('\n')

    print ("[DUMPSTATS] IPC Plot")
    os.system('gnuplot -e "set terminal dumb; plot \'ipc_dump.tsv\' smooth unique"')
    os.system('rm ipc_dump.tsv')
    return
  
  def print_mips(self):
    # [a,b]: [instruction_count, time count]
    mips_performance = {"detailed":[0.0,0.0], "fastforward":[0.0,0.0]}

    for i in range(self.region_count):
      # get stats for each region, not only representative regions
      stats = self.stats_per_region[i]

      cluster_id = self.cluster_labels[i];
      cluster_stats = self.cluster_stats_dict[cluster_id]

      if (stats.is_roi):
        # this region is simulated in detail
        sim_mode = "detailed"
      else:
        sim_mode = "fastforward"

      mips_performance[sim_mode][0] += sum(cluster_stats.instruction_count_per_core)
      mips_performance[sim_mode][1] += max(stats.walltime_per_core)
      
    # avoid division by zero, when whole benchmark is simulated in fastforward/detailed mode
    if (mips_performance["detailed"][1] == 0):
        mips_performance["detailed"][0] = 0.000000001
        mips_performance["detailed"][1] = 1
    if (mips_performance["fastforward"][1] == 0):
        mips_performance["fastforward"][0] = 0.000000001
        mips_performance["fastforward"][1] = 1

    print ("[WARNING] MIPS Calculation is erroneous!")
    print ("[DUMPSTATS] Detailed Simulation MIPS: ", mips_performance["detailed"][0] / mips_performance["detailed"][1])
    print ("[DUMPSTATS] FForward Simulation MIPS: ", mips_performance["fastforward"][0] / mips_performance["fastforward"][1])
    print ("[DUMPSTATS] Overall  Simulation MIPS: ", (mips_performance["detailed"][0]+mips_performance["fastforward"][0]) / \
                                                    (mips_performance["detailed"][1]+mips_performance["fastforward"][1]))
    print ("[DUMPSTATS] Simulated: ", mips_performance["detailed"][0] / mips_performance["fastforward"][0] * 100, " % in detail.")

    return

  def print_overall_runtime(self):
    total_time = 0.0
    for i in range(self.region_count):
      # get stats
      cluster_id = self.cluster_labels[i]
      mtng_stats = self.cluster_stats_dict[cluster_id]

      mtng_stats_real = self.stats_per_region[i]

      runtime1 = float(mtng_stats.time0)
      inscount1 = float(sum(mtng_stats.instruction_count_per_core))

      # scaling
      inscount1_real = float(sum(mtng_stats_real.instruction_count_per_core))
      runtime1 = runtime1  / inscount1 * inscount1_real; # constant scale factor to approximate
    
      print( "region_" + str( i) ,runtime1/1e15 )
      ###############################
      total_time += runtime1

    print ("[DUMPSTATS] Overall Simulated Runtime: ", total_time/1e15, ' seconds')
    return total_time

    
  def plot_runtime(self):

    plotfile = "%(output_sniper_validate_dir)s/runtime_dump.tsv"%self.config
    with open("%s"%plotfile,"w") as f:
      for i in range(self.region_count):
        # get stats
        cluster_id = self.cluster_labels[i]
        stats = self.cluster_stats_dict[cluster_id]
        f.write(str(i))
        f.write('\t')
        f.write(str(float(stats.time0)/1e15))
        f.write('\n')

    # print "[WARNING] RUNTIME calculation is erroneous!"
    print ("[DUMPSTATS] Runtime Plot")
    os.system('gnuplot -e "set terminal dumb; plot \'%s\' smooth unique"'%plotfile)
#    os.system('rm runtime_dump.tsv')

    return


  def plot_runtime_error(self):
    total_time_mtng = 0.0
    total_time_validate = 0.0
    total_inscount_mtng = 0.0
    total_inscount_validate = 0.0
    # for gnuplot rescaling e = 0.0
    error_high = 0.0
    error_low = 0.0
    plotfile = "%(output_sniper_validate_dir)s/runtime_error_dump.tsv"%self.config
    with open("%s"%plotfile,"w") as f:
      for i in range(self.region_count):
        # get mtng stats
        cluster_id = self.cluster_labels[i]
        mtng_stats = self.cluster_stats_dict[cluster_id]
        mtng_stats_real = self.stats_per_region[i]

        # get valdiate stats
        m1 = "marker-"+str(i)+"-0"
        m2 = "marker-"+str(i)+"-1"
        validate_stats = PerformStats(self.config,(m1,m2),validate=True);

        # TODO: Is this correct?
        runtime1 = float(mtng_stats.time0)
        runtime2 = float(validate_stats.time0)
        inscount1 = float(sum(mtng_stats.instruction_count_per_core))
        inscount2 = float(sum(validate_stats.instruction_count_per_core))

        # scaling
        inscount1_real = float(sum(mtng_stats_real.instruction_count_per_core))
        runtime1 = runtime1  / inscount1 * inscount1_real; # constant scale factor to approximate
        # inscount1_real = inscount1

        total_time_mtng += runtime1
        total_time_validate += runtime2
        runtime_error = (runtime1 - runtime2) / runtime2 * 100.0

        total_inscount_mtng += inscount1_real
        total_inscount_validate += inscount2
        inscount_error = (inscount1_real - inscount2) / inscount2 * 100.0

        # gnuplot yscale
        error_high = max(runtime_error,error_high)
        error_low = min(runtime_error,error_low)

        f.write(str(i))
        f.write('\t')
        f.write(str(runtime_error))
        f.write('\t')
        f.write(str(runtime1))
        f.write('\t')
        f.write(str(runtime2))
        f.write('\n')

    print ("[WARNING] RUNTIME error calculation is erroneous!")
    print ("[DUMPSTATS] Mtng Simulated runtime: ", total_time_mtng / 1e15, ' seconds')
    print ("[DUMPSTATS] Validation Simulated runtime: ", total_time_validate / 1e15, ' seconds')
    print ("[DUMPSTATS] Overall Runtime Error: ", (total_time_mtng - total_time_validate) / total_time_validate * 100, ' %')
    print ("[DUMPSTATS] Runtime Error Plot")

    os.system('gnuplot -e "set terminal dumb; set yrange [%d:%d]; plot \'%s\' smooth unique"'%(error_low,error_high,plotfile))
#    os.system('rm runtime_error_dump.tsv')

  def plot_runtime_error_vs_inscount_error(self):
    total_time_mtng = 0.0
    total_time_validate = 0.0
    total_inscount_mtng = 0.0
    total_inscount_validate = 0.0
    y_high = 0
    y_low = 0

    plotfile = "%(output_sniper_validate_dir)s/runtime_error_vs_inscount_error_dump.tsv"%self.config
    with open("%s"%plotfile,"w") as f:
      for i in range(self.region_count):
        # get mtng stats
        cluster_id = self.cluster_labels[i]
        mtng_stats = self.cluster_stats_dict[cluster_id]

        # get valdiate stats
        m1 = "marker-"+str(i)+"-0"
        m2 = "marker-"+str(i)+"-1"
        validate_stats = PerformStats(self.config,(m1,m2),validate=True);

        # TODO: Is this correct?
        runtime1 = float(mtng_stats.time0)
        runtime2 = float(validate_stats.time0)
        total_time_mtng += runtime1
        total_time_validate += runtime2
        runtime_error = (runtime1 - runtime2) / runtime2 * 100.0

        inscount1 = float(sum(mtng_stats.instruction_count_per_core))
        inscount2 = float(sum(validate_stats.instruction_count_per_core))
        total_inscount_mtng += inscount1
        total_inscount_validate += inscount2
        inscount_error = (inscount1 - inscount2) / inscount2 * 100.0

        y_high = max(y_high,runtime_error)
        y_low = min(y_low,runtime_error)

        if (self.stats_per_region[i].is_roi):
            print("===Detailed Region [%d]==="%i)
        else:
            print("===FFwarded Region [%d]==="%i)
        print ("Runtime error: ", runtime_error)
        print ("Instruction count error: ", inscount_error)
        print ("Instructions simulated for mtng:" , mtng_stats.instruction_count_per_core)
        print ("Instructions simulated for detailed simulator:" , validate_stats.instruction_count_per_core)

        f.write(str(int(inscount_error)))
        f.write('\t')
        f.write(str(int(runtime_error)))
        f.write('\n')

    print ("[DUMPSTATS] Mtng Simulated runtime: ", total_time_mtng / 1e15, ' seconds')
    print ("[DUMPSTATS] Mtng Simulated inscount: ", total_inscount_mtng)
    print ("[DUMPSTATS] Validation Simulated runtime: ", total_time_validate / 1e15, ' seconds')
    print ("[DUMPSTATS] Validation Simulated inscount: ", total_inscount_validate)
    print ("[DUMPSTATS] Overall Runtime Error: ", (total_time_mtng - total_time_validate) / total_time_validate * 100, ' %')
    print ("[DUMPSTATS] Overall inscount Error: ", (total_inscount_mtng - total_inscount_validate) / total_inscount_validate * 100, ' %')
    print ("[DUMPSTATS] Runtime Error Plot")

    os.system('gnuplot -e "set terminal dumb; set yrange [%d:%d]; set xlabel \'inscount error\' ; set ylabel \'runtime error\'; plot \'%s\'"'%(int(y_low),int(y_high),plotfile))
#    os.system('rm runtime_error_dump.tsv')
