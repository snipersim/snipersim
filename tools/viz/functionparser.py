#!/usr/bin/env python2
# coding: utf-8
import subprocess, cStringIO

#script that takes a rtntrace file and parses it

#format of inputdata:

#example : eip  name	source	calls	bits_used	bits_total	instruction_count	nonidle_elapsed_time	l2miss	l3miss	global_instructions	global_nonidle_elapsed_time	cpiBase	cpiBranchPredictor	cpiMem	ADDPD	ADDSD	ADDSS	ADDPS	SUBPD	SUBSD	SUBSS	SUBPS	MULPD	MULSD	MULSS	MULPS	DIVPD	DIVSD	DIVSS	DIVPS

#Demangle C++ names:

def cppfilt(name):
    args = ['c++filt']
    args.extend([name])
    pipe = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    stdout, _ = pipe.communicate()
    return stdout.replace('\n','')			

	
#read inputdata
def parseFunctions(inputfile = None, inputdata = None):
  functiondata = []
  total = dict(
    calls = 0,
    instruction_count = 0,
    nonidle_elapsed_time = 0,
    l3miss = 0
  )
  if inputfile:
    f=open(inputfile, "r")
  elif inputdata:
    f=cStringIO.StringIO(inputdata)
  else:
    return None,None
  i=0
  idnr=0
  name2idx={}
  for line in f:
    output=line.rstrip(' \n').split('\t')
    if i == 0:
      name2idx = dict(map(lambda a:(a[1],a[0]), enumerate(output)))
    #only functions that execute 1 or more instructions are stored
    if i > 0 and float(output[name2idx['instruction_count']]) >0:
      d = dict(
	eip				=output[name2idx['eip']], 
	name				=output[name2idx['name']],
        name_clean			=cppfilt(output[name2idx['name']]), 
	source				=output[name2idx['source']], 
	calls				=float(output[name2idx['calls']]),
        bits_used			=float(output[name2idx['bits_used']]),
        bits_total			=float(output[name2idx['bits_total']]),
	instruction_count		=float(output[name2idx['instruction_count']]), 
        nonidle_elapsed_time 		=float(output[name2idx['nonidle_elapsed_time']]),
	l3miss				=float(output[name2idx['l3miss']]),
        global_instructions		=float(output[name2idx['global_instructions']]),
        global_non_idle_elapsed_time	=float(output[name2idx['global_nonidle_elapsed_time']]),
        cpiBase				=float(output[name2idx['cpiBase']]),
        cpiBranchPredictor		=float(output[name2idx['cpiBranchPredictor']]),
	cpiMem				=float(output[name2idx['cpiMem']]),
	addpd				=float(output[name2idx['ADDPD']]),
	addsd				=float(output[name2idx['ADDSD']]),
	addss				=float(output[name2idx['ADDSS']]),
	addps				=float(output[name2idx['ADDPS']]),
	subpd                           =float(output[name2idx['SUBPD']]),
        subsd                           =float(output[name2idx['SUBSD']]),
        subss                           =float(output[name2idx['SUBSS']]),
        subps                           =float(output[name2idx['SUBPS']]),
	mulpd                           =float(output[name2idx['MULPD']]),
        mulsd                           =float(output[name2idx['MULSD']]),
        mulss                           =float(output[name2idx['MULSS']]),
        mulps                           =float(output[name2idx['MULPS']]),
	divpd                           =float(output[name2idx['DIVPD']]),
        divsd                           =float(output[name2idx['DIVSD']]),
        divss                           =float(output[name2idx['DIVSS']]),
        divps                           =float(output[name2idx['DIVPS']]),
	time_won_back			=0,
	optimizations			=[],
        id				=idnr
	)
      try:
        d['l2miss'] = output[name2idx['l2miss']]
      except KeyError:
        pass
      functiondata.append(d)

      total["calls"]+=float(output[name2idx['calls']])
      total["instruction_count"]+=float(output[name2idx['instruction_count']])
      total["nonidle_elapsed_time"]+=(float(output[name2idx['nonidle_elapsed_time']]))
      total["l3miss"]+=float(output[name2idx['l3miss']])

      idnr+=1
    i+=1
  f.close() # dont forget to close the file
  return functiondata, total

def parseFunctionsDict(d):
  da = zip(*d.items())
  return parseFunctions(inputdata = ('\t'.join(map(str,da[0]))+'\n'+'\t'.join(map(str,da[1]))))
