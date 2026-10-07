#!/usr/bin/env python3
#
# Experiment driver for the Pac-Sim paper's comparisons.  NOT an in-tree entry
# point, and it does not run as it stands:
#
#   - it builds `--mtng=N` arguments for run-sniper, selecting between the
#     methodologies the paper compares against (1 Pac-Sim, 2 hybrid, 100
#     SMARTS, 200 time-based, 1004 fast-forward, 1005 analysis, 1006/1007 MTR).
#     run-sniper has never accepted --mtng, in any revision of this repository;
#     these target a separate driver.
#   - it imports iqmtnglib, which is not in this tree, and defaults to remote
#     paths on a particular cluster.
#
# The --mtng spellings are left alone deliberately: they are a protocol with
# something outside this repository, so renaming them here would only invent a
# name nothing answers to.  Everything that is ours has moved to "pacsim"; MTNG
# was the earlier name for the same methodology.
#
# To run Pac-Sim from this tree, use `run-sniper --pacsim` (or
# --dynamic-sampling); for the estimate afterwards, pacsim.py.
import os, sys

# class to hold representative stats

#from prepare import npbdatasize
from . import dumpstats
from . import pacsim_dump
from . import pacsim_dumpjson
import json
import copy
import argparse
#from prepare import get_benchmarks,get_cmd,get_binary,specdatasize,icc_postfix,gcc_postfix
from .my_utils import check_dir
def get_startsim_parms( bench,arch_cfg ,is_full_run ,scheduler,ncores,dst_dir,enable_dvfs,cluster_threshold,region_size,minimum_region_size,smarts_ffw_num,compiler):
#  scheduler="static"
  sniper_args = []
  sniper_args += ['-v']
  sniper_args += ['--sift']
#  sniper_args += ['--gdb']
#  sniper_args += ['--appdebug']
  sniper_args += ['--roi']
  sniper_args += ['-sprogresstrace:10000000']
  sniper_args += ['-gtraceinput/timeout=2000']
  if compiler == "gcc" and scheduler=="static":
      sniper_args += ['-gscheduler/type=%(scheduler)s' %locals()]
#  sniper_args += ['--trace-args="-filter_exclude_lib libiomp5.so"']
  sniper_args += ['-gcore/hook_periodic_ins/filter_spinloops=true']
  sniper_args += ['--trace-args="-flow 1000"']
  sniper_args += [ '-c rob']
#  sniper_args += [ '-g perf_model/core/rob_timer/in_order=true']#, '--viz']
  sniper_args += ['-c%s' % arch_cfg]
  sniper_args += ['-d %s'%dst_dir ]
  sniper_args += ['-n %d'%ncores]

  if( is_full_run == "mtng"):
    sniper_args += ['--mtng=1']
  elif is_full_run == 'emu':
    sniper_args += ['--emu']
  elif is_full_run == "mtnghybrid":
    sniper_args += ['--mtng=2']
  elif is_full_run == "smarts":
    sniper_args += ['--mtng=100']
    if region_size == 50000:
        region_size=1000 ##smarts default region size is 1M
  elif is_full_run == "time":
    sniper_args += ['--mtng=200']
    print (region_size)
    if region_size == 50000:
#        region_size = 1000
        if bench == "wrf":
            region_size = 5000
            region_size = 2000###wrftime100
        else:
            region_size = 10000
        minimum_region_size = region_size
  elif( is_full_run == "full"):
    pass
  elif is_full_run == "ffw":
    sniper_args += ['--mtng=1004']
  elif is_full_run == "analysis":
    sniper_args += ['--mtng=1005']
  elif is_full_run == "mtr":
    sniper_args += ['--mtng=1006']
  elif is_full_run == "puremtr":
    sniper_args += ['--mtng=1007']

  else:
    print("Mode %s not support"%is_full_run)
    exit()

  if enable_dvfs:
      sniper_args+=['--dvfs']
  sniper_args += ["--cluster-threshold=%f"%cluster_threshold]
  sniper_args += ["--region-size=%d"%region_size]
  sniper_args += ["--minimum-region-size=%d"%minimum_region_size]
  sniper_args += ["--smarts-ffw-num=%d"%smarts_ffw_num]
  sniper_args += ['-s markers:stats']
  print(sniper_args)
  return sniper_args


def get_args():
    parser = argparse.ArgumentParser(description="Analysing data for detail mode")

    parser.add_argument("--compiler" , type=str, default="gcc",help=" compiler we used ")
    parser.add_argument("--mode" , type=str, default="full",help=" sim mode : full or mtng (sampled simulation) ")
    parser.add_argument("--bench" , type=str, default="matmul",help=" decide which benchmarks to execute ")
    parser.add_argument("--bench-idx" , type=int, default=0,help=" index of inside bench ")
    parser.add_argument("--v" , type=int, default=10,help=" version ")
    parser.add_argument("--dst-dir" , type=str, default="./",help=" destination dir ")
    parser.add_argument("--ncores" , type=int, default=8,help=" ncores ")
    parser.add_argument("--region-size" , type=int, default=50000,help=" sampled region size ")
    parser.add_argument("--minimum-region-size" , type=int, default=20000,help=" minimum sampled region size ")
    parser.add_argument("--smarts-ffw-num" , type=int, default=10,help=" smarts consecutive ffw region nums ")
    parser.add_argument("--dataset" , type=str, default="train",help=" data set ")
    parser.add_argument("--local" , type=str, default="remote",help=" options: bare, local, remote, default to submit job to job system machine")
    parser.add_argument("--check" ,action="store_true" , default=False,help=" check the final result")
    parser.add_argument("--parallel" ,action="store_true" , default=False,help=" check the final parallel result")

    parser.add_argument("--cluster-threshold" ,type=float , default=0.05,help=" cluster threshold")
    parser.add_argument("--check-type" ,type=str , default="time",help=" check type for final result")
    parser.add_argument("--force" ,action="store_true" , default=False,help=" force to reload data from server")
    parser.add_argument("--dvfs" ,action="store_true" , default=False,help=" dvfs enable or not")
    parser.add_argument("--check-dir" , type=str , default=None,help=" user define the check dir")
#    parser.add_argument("--check" , type=str , default='non',help=" check the final result; json or sql")
                                                     #gainestown
    parser.add_argument("--arch" ,type=str , default="gainestown",help=" target simulate architecture")
    parser.add_argument("--scheduler" ,type=str , default="static",help=" scheduler")
    parser.add_argument("--home-dir" ,type=str , default= os.path.join(os.getenv("HOME"), "data"),help=" base dir")
    parser.add_argument("--remote-home-dir" ,type=str , default='/home/rsch/comparch/comparch/results/mtng/',help=" remote base dir")
    args = parser.parse_args()
    return args
def exec2( bench,cmd , args , datasize):
    if_local = args.local
#    gcc_postfix = ".gcc-linux-x86-bdx-o3-m64"
    #icc_postfix = ".icc-o2-m64"
    if args.compiler == "icc":
        postfix = icc_postfix
    else:
        postfix = gcc_postfix
    if mode == "bare":
        print(cmd)
        os.system(cmd)
    else:

        sniper_args = get_startsim_parms(bench, args.arch ,args.mode, args.scheduler ,args.ncores,args.dst_dir,args.dvfs,args.cluster_threshold,args.region_size,args.minimum_region_size,args.smarts_ffw_num,args.compiler)
        check_type = args.check_type
        if check_type != "pin":
            check_type = ""
        if if_local == "remote":

            from iqmtnglib import mtng_graphite_submit,get_job_name,get_jobid_from_name,mtng_pintool_submit
# mtng_graphite_submit( jobname ,  inputsize,nthreads,gitid ):
            inputsize = args.dataset
            if datasize not in npbdatasize:
                jobname = get_job_name( args.arch,args.mode,args.scheduler,args.ncores, bench,cmd,version_local,inputsize,check_type)
            else:
                cmd = cmd+datasize
                jobname = get_job_name( args.arch,args.mode,args.scheduler,args.ncores, bench,cmd,version_local,inputsize,check_type)
            files = ["prepare.py","postprocess","my_utils.py"]
            for i,_ in enumerate(files):
                files[i] = os.path.join(os.path.dirname(__file__),files[i] )
            if check_type == "pin":
                mtng_pintool_submit( jobname ,args.dataset,args.ncores,cmd, cmd, bench,postfix,files)
            else:
                mtng_graphite_submit( jobname ,args.dataset,args.ncores,cmd, sniper_args, bench,files,postfix)
        elif if_local == "local":
            sniper_cmd = "run-sniper " + " ".join(sniper_args)
            cmd = sniper_cmd + " -- " + cmd
            print(cmd)
            os.system(cmd)
import hashlib
def get_job_name( arch_cfg ,is_full_run ,scheduler,ncores,bench,cmd,version_local,inputsize,check_type):
    print(cmd)
    hash_cmd =  int(hashlib.sha1(cmd.encode("utf-8")).hexdigest(), 16) % (10 ** 8)
#    if is_full_run == "mtng":
#        version_local = mtng_version
#    else:
#        version_local = full_version
    if inputsize == "ref":
#        if check_type != "":
#            is_full_run = "full"
        jobname = "%(arch_cfg)s-%(is_full_run)s-%(scheduler)s-n%(ncores)s-%(bench)s-ref-%(hash_cmd)s-v%(version_local)s"%locals()
    else:
        jobname = "%(arch_cfg)s-%(is_full_run)s-%(scheduler)s-n%(ncores)s-%(bench)s-%(hash_cmd)s-v%(version_local)s"%locals()
    if check_type != "time" and check_type != "":
        jobname += "-"+check_type
    return jobname


def check_result( bench,ci,cmd,args):
    datasize = args.dataset
    if args.check_type == "pin":
        args.mode="full"
#        args.mode="mtng"
        branchmode = "mtng"
    else:
        branchmode = args.mode
    print(args.mode)
    l2missrate = 0
    if args.check_dir == None:
# mtng_graphite_submit( jobname ,  inputsize,nthreads,gitid ):
        if datasize not in npbdatasize:
            jobname = get_job_name( args.arch,args.mode,args.scheduler,args.ncores, bench,cmd,version_local,datasize,args.check_type)
        else:
            cmd = cmd+datasize
            jobname = get_job_name( args.arch,args.mode,args.scheduler,args.ncores, bench,cmd,version_local,datasize,args.check_type)
        home_dir = args.home_dir
        full_program_path = os.path.join( home_dir , jobname )
        #print(full_program_path, os.path.isdir( full_program_path ))
        datadir = os.path.join( full_program_path ,'mtng_output','sniper')
        print(datadir)
        #exit(1)
        if args.local!="local" and (not os.path.isdir(datadir ) or (args.force) ):
            from iqmtnglib import mtng_graphite_submit,get_jobid_from_name
            if not args.local == "local" :
                jobid = get_jobid_from_name(jobname)
            else:
                jobid = 0
            print( jobname," jobid: ", jobid )

            if jobid != None:


                #server_url = "comparch@xlog0"
                remote_home_dir = args.remote_home_dir
                check_dir(full_program_path)
                check_dir(datadir)
                print(jobid)
                #os.system( "scp -r %(server_url)s:%(remote_home_dir)s/%(jobid)s/mtng_output/ %(full_program_path)s/"%locals() )
                txt_files = ["intra_mtng.json","sim.stats.sqlite3","sim.cfg","sim.info","full.json"]
                bin_files = []
#                bin_files = ["sim.stats.sqlite3",]
                for file_ in txt_files:
                    cmd_tmp = "iqresults -f %(file_)s %(jobid)s  "%locals()
                    file_path = "%(full_program_path)s/mtng_output/sniper/%(file_)s"%locals()
                    print(cmd_tmp)
#                    os.system(cmd_tmp )
                    output = os.popen(cmd_tmp)
                    print(file_path)
                    with open(file_path,"w") as f:
                        f.write( output.read() )
                for file_ in bin_files:
                    output = os.popen( "iqresults  -f %(file_)s -a %(jobid)s "%locals() )

                    file_path = "%(full_program_path)s/mtng_output/sniper/%(file_)s"%locals()
                    with open( file_path,"wb" ) as f:
                        f.write( output.read() )
    else:
        datadir = args.check_dir
    print( datadir )
    time_mtng = 0
    wall_mtng = 0
    if os.path.isdir( datadir):
        if branchmode == "mtng" or branchmode=="hybridmtng" or branchmode == "analysis" or branchmode=="time":

#            if args.check == 'sql': ##old inference
#                full_program_path = os.path.join(full_program_path,"mtng_output","sniper")
#                print(full_program_path)
#                time_mtng, wall_mtng = pacsim_dump.get_full_time( full_program_path,os.environ.get("SNIPER_ROOT") )
#            elif
             if args.check :
                which_bench = "%s_%s"%(bench,ci)
                if which_bench in threshold:
                    pacsim_dumpjson.threshold_in = threshold[which_bench]
                full_program_path = datadir
                if args.check_type == "time":
                    #print( full_program_path )
                    try :
                        time_mtng, wall_mtng,l2missrate = pacsim_dumpjson.get_full_time( full_program_path,os.environ.get("SNIPER_ROOT"),args.parallel )
                        print(wall_mtng)
                        print(l2missrate)
                    except IndexError:
                        print( "index error")
                    except ValueError:
                        print( "value error")
                elif   args.check_type == "pin":

                    time_mtng, wall_mtng = pacsim_dumpjson.get_ref_full_version_time( full_program_path,os.environ.get("SNIPER_ROOT"),args.parallel )
                elif args.check_type == "marker":
                    time_mtng, wall_mtng = pacsim_dumpjson.get_marker_distribution( full_program_path,os.environ.get("SNIPER_ROOT") ) #barrierpoint, looppoint

                elif args.check_type == "regionsize":
                    cmd = "cp -r %s %s"%( full_program_path,os.path.join("~/tmp/collect/","%s_%d"%(bench,ci)))
                    print(cmd)
                    os.system(cmd)
                elif args.check_type == "predict":
                    try:
                        time_mtng, wall_mtng = pacsim_dumpjson.predictaccuracy( full_program_path,os.environ.get("SNIPER_ROOT") ) #barrierpoint, looppoint
                    except ValueError:
                        time_mtng,wall_mtng=[0,0,0,0],0
                    print(time_mtng,wall_mtng)
                elif args.check_type == "extrapolate":
                    time_mtng, wall_mtng = pacsim_dumpjson.extrapolatedistribution( full_program_path,os.environ.get("SNIPER_ROOT") ) #barrierpoint, looppoint
                    print(time_mtng)
                elif args.check_type == "timedistribute":
                    try:
                        time_mtng, wall_mtng = pacsim_dumpjson.timedistribution( full_program_path,os.environ.get("SNIPER_ROOT") ) #barrierpoint, looppoint
                    except ValueError:
                        time_mtng = [0,0,1]
                        wall_mtng=0
                    time_mtng2 = copy.copy(time_mtng)
                    time_mtng = time_mtng2[0]
                    wall_mtng = time_mtng2[1]

#                    time_mtng = time_mtng2[0] / time_mtng2[2] * 100
#                    wall_mtng = time_mtng2[1] / time_mtng2[2] * 100
                    print(time_mtng)

        elif args.mode=="smarts":
             if args.check :
                full_program_path = datadir
                if args.check_type == "time":
                    time_mtng, wall_mtng = smarts_dumpjson.get_full_time( full_program_path,os.environ.get("SNIPER_ROOT") )

        else:
            full_program_path = datadir
            time_mtng, wall_mtng,l2missrate = dumpstats.get_full_time( full_program_path,os.environ.get("SNIPER_ROOT") )

        print("final %s %s %s"%(time_mtng,wall_mtng,l2missrate))
    else:
        print( jobname , "Not Exit or Finish" )
    return time_mtng,wall_mtng,l2missrate

def print_all_array(benchs, arrays):
    for bi,bench in enumerate(benchs):
        str1="%s\t"%bench

        array = arrays[bi]
        if array==0:
            pass
        else:
            for elem in array:
                str1 +="%s\t"%elem
        print("final result",str1)
def print_all(benchs, walls,times,l2s):
    for bi, bench in enumerate( benchs):
        str1="results: %s\t%s\t%s\t%s"%(bench,times[bi],walls[bi],l2s[bi])
        print( str1 )
def print_predict(benchs, times):
    for bi, bench in enumerate( benchs):
        str1 = bench
        #print(type(times[bi]),)
        if( isinstance(times[bi],list) ):
            if(len(times[bi]) != 0  ):
                str1="%s\t%s"%(bench,"\t".join( map(str,times[bi])))

        print( str1 )

version_local=10
threshold = dict()
if __name__ == "__main__":
    args = get_args()
 #   gcc_postfix = "_base.gcc-linux-x86-bdx-o3-m64"
#    icc_postfix = ".icc-o2-m64"
#    icc_postfix=".icc18.0.gO2avx"
    if args.compiler == "icc":
        postfix = icc_postfix
    else:
        postfix = gcc_postfix

    if args.check:
        time_mtngs = []
        wall_mtngs = []
        l2_mtngs = []
        bench_mtngs = []
    mode = args.mode
    version_local = args.v
    threshold["pop2_0"] = 1
    if args.bench == "all":
        benchmarks = get_benchmarks()
#        print(benchmarks)
        benchmarks_done = dict()
        for bench in benchmarks:
            cmdset =  get_cmd( bench, data_type = args.dataset )
            binary = get_binary( bench,args.dataset )
            if binary == None:
                continue
            for ci,cmd1 in enumerate(cmdset):

                if args.dataset in specdatasize:
                    cmd1 = "./%s%s %s"%(binary,postfix,cmd1)
                else:
                    cmd1 = "./%s %s"%(binary,cmd1)

                if cmd1 in benchmarks_done:
                    continue
                else:
                    benchmarks_done[cmd1] = 1
#                print(cmdset)
                if args.check :
                    time_mtng,wall_mtng,l2missrate = check_result( bench,ci,cmd1,args)
                    time_mtngs.append(time_mtng)
                    wall_mtngs.append(wall_mtng)
                    l2_mtngs.append(l2missrate)
                    bench_mtngs.append(bench+"."+str(ci))
                else:
#                    pass
#                    print("")
                    #print(bench,cmd1,args)
                    exec2(bench,cmd1,args,args.dataset)
        if args.check:
            if args.check_type == "extrapolate":
                print_all_array( bench_mtngs, time_mtngs )
            if args.check_type == "predict":
                print_predict( bench_mtngs, time_mtngs )
            else:

                print_all( bench_mtngs,wall_mtngs,time_mtngs,l2_mtngs )
    else:
        bench = args.bench
        cmdset_tmp = get_cmd(bench,data_type=args.dataset)
        #print(cmdset_tmp)
        binary = get_binary( bench,args.dataset )
        if binary != None:

            cmdset = []
            for tmp in cmdset_tmp:
                if args.dataset in specdatasize:
                    cmdset1 = "./%s%s %s"%(binary,postfix,tmp)
                else:
                    cmdset1 = "./%s %s"%(binary,tmp)
                cmdset.append(cmdset1)

#            print(cmdset)
            if args.check :
                check_result( bench,args.bench_idx,cmdset[args.bench_idx],args)
            else:
                exec2( bench,cmdset[args.bench_idx],args,args.dataset )








