#!/usr/bin/env python
# coding: utf-8
import subprocess

#script that takes a rtntrace file and parses it

#format of inputdata:

#example : eip  name	source	calls	bits_used	bits_total	instruction_count	core_elapsed_time	nonidle_elapsed_time	fp_addsub	fp_muldiv	l3miss	global_instructions	global_nonidle_elapsed_time	cpiBase	cpiBranchPredictor	cpiMem	ADDPD	ADDSD	ADDSS	ADDPS	SUBPD	SUBSD	SUBSS	SUBPS	MULPD	MULSD	MULSS	MULPS	DIVPD	DIVSD	DIVSS	DIVPS

#Demangle C++ names:

def cppfilt(name):
    args = ['c++filt']
    args.extend([name])
    pipe = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    stdout, _ = pipe.communicate()
    return stdout.replace('\n','')			

	
#read inputdata
def parseFunctions(inputfile):
  functiondata = []
  total = dict(
    calls = 0,
    instruction_count = 0,
    core_elapsed_time = 0,
    nonidle_elapsed_time = 0,
    fp_addsub = 0,
    fp_muldiv = 0,
    l3miss = 0
  )
  f=open(inputfile, "r" )
  i=0
  idnr=0
  for line in f:
    file=line  
    output=file.split('\t')
    #only functions that execute 1 or more instructions are stored
    if i > 0 and float(output[6]) >0:
      functiondata.append(dict(
	eip				=output[0], 
	name				=output[1],
        name_clean			=cppfilt(output[1]), 
	source				=output[2], 
	calls				=float(output[3]),
        bits_used			=float(output[4]),
        bits_total			=float(output[5]),
	instruction_count		=float(output[6]), 
	core_elapsed_time		=float(output[7]),
        nonidle_elapsed_time 		=float(output[8]),
	fp_addsub			=float(output[9]),
        fp_muldiv			=float(output[10]), 
	l3miss				=float(output[11]),
        global_instructions		=float(output[12]),
        global_non_idle_elapsed_time	=float(output[13]),
        cpiBase				=float(output[14]),
        cpiBranchPredictor		=float(output[15]),
	cpiMem				=float(output[16]),
	addpd				=float(output[17]),
	addsd				=float(output[18]),
	addss				=float(output[19]),
	addps				=float(output[20]),
	subpd                           =float(output[21]),
        subsd                           =float(output[22]),
        subss                           =float(output[23]),
        subps                           =float(output[24]),
	mulpd                           =float(output[25]),
        mulsd                           =float(output[26]),
        mulss                           =float(output[27]),
        mulps                           =float(output[28]),
	divpd                           =float(output[29]),
        divsd                           =float(output[30]),
        divss                           =float(output[31]),
        divps                           =float(output[32]),
	time_won_back			=0,
	optimizations			=[],
        id				=idnr
	))
      total["calls"]+=float(output[3])
      total["instruction_count"]+=float(output[6])
      total["core_elapsed_time"]+=(float(output[7]))
      total["nonidle_elapsed_time"]+=(float(output[8]))
      total["fp_addsub"]+=float(output[9])
      total["fp_muldiv"]+=float(output[10])
      total["l3miss"]+=float(output[11])

      idnr+=1
    i+=1
  f.close() # dont forget to close the file
  return functiondata, total
