#!/usr/bin/env python2
# coding: utf-8

import copy, asohelper

#This module contains all the optimization modules.
#Each module takes a function (and some the config file), copies the function
#and modifies the properties.
#Modules can easily be combined. E.g. BranchModule(MemoryModule(function))

#==================================================================================
#			Instruction Level Parallellism
#==================================================================================
def ILPModule(function, config):
  frequency                     = float(config["perf_model/core/frequency"])
  optimizedfunction             = copy.deepcopy(function)
  current_time                  = function["nonidle_elapsed_time"]
  current_instruction_count     = function["instruction_count"]
  current_cpiBase               = function["cpiBase"] #cpiBase is in femtoseconds
  #calculate CPI of 0.25 as (instructions/4)*clock_period 
  clock_period                  = (1/frequency)*1e6 #femtoseconds
  new_cpiBase                   = (current_instruction_count / 4)*clock_period
  time_gain                     = current_cpiBase - new_cpiBase
  if time_gain > 0:
    #changing properties of function
    optimizedfunction["cpiBase"]                = new_cpiBase
    optimizedfunction["time_won_back"]          += time_gain
    optimizedfunction["nonidle_elapsed_time"]   -= time_gain
    optimizedfunction["optimizations"].append(dict(optimization="ILP",timegain=time_gain))
    optimizedfunction["originalfunction"]       = function
  return optimizedfunction


#==================================================================================
#			Branching
#==================================================================================
def BranchModule(function):
  optimizedfunction             = copy.deepcopy(function)
  cpiBranchPredictor            = function["cpiBranchPredictor"]
  #changing properties of function
  #we can now remove the branchPredictor cpicomponent
  optimizedfunction["cpiBranchPredictor"]       = 0
  optimizedfunction["time_won_back"]            += cpiBranchPredictor
  optimizedfunction["nonidle_elapsed_time"]     -= cpiBranchPredictor
  optimizedfunction["optimizations"].append(dict(optimization="Branch", timegain=cpiBranchPredictor))
  optimizedfunction["originalfunction"]         = function
  return optimizedfunction

#==================================================================================
#			Removing Non-floating point instructions
#==================================================================================
def NonFPModule(function):
  instruction_count     = function["instruction_count"]
  nonidle_elapsed_time  = function["nonidle_elapsed_time"]
  fp_addsub             = asohelper.get_fp_addsub(function)
  fp_muldiv             = asohelper.get_fp_muldiv(function)
  cpiBase               = function["cpiBase"]
  cpiMem                = function["cpiMem"]
  cpiBranchPredictor    = function["cpiBranchPredictor"]
  cpiOther              = nonidle_elapsed_time - cpiBase - cpiMem - cpiBranchPredictor
  fp_instructions       = fp_addsub + fp_muldiv
  non_fp_instructions   = instruction_count - fp_instructions
  non_fp_fraction       = non_fp_instructions / instruction_count #the fraction of non-fp instructions
  if instruction_count > 0:
    non_fp_time         = non_fp_fraction*(cpiBase + cpiMem + cpiBranchPredictor + cpiOther)
  else:
    non_fp_time         = 0
  optimizedfunction     = copy.deepcopy(function)
  if non_fp_time > 0:
    #changing properties of function
    optimizedfunction["instruction_count"]      -=non_fp_instructions
    optimizedfunction["time_won_back"]          +=non_fp_time
    optimizedfunction["nonidle_elapsed_time"]   -=non_fp_time
    optimizedfunction["cpiBase"]                -=non_fp_fraction*cpiBase
    optimizedfunction["cpiMem"]                 -=non_fp_fraction*cpiMem
    optimizedfunction["cpiBranchPredictor"]     -=non_fp_fraction*cpiBranchPredictor
    optimizedfunction["optimizations"].append(dict(optimization="NonFP", timegain=non_fp_time))
    optimizedfunction["originalfunction"]       = function

  return optimizedfunction


#==================================================================================
#			Vectorization module
#==================================================================================
def VectorizationModule(function):
  optimizedfunction             = copy.deepcopy(function)
  instruction_count             = function["instruction_count"]
  nonidle_elapsed_time          = function["nonidle_elapsed_time"]
  fp_addsub                     = asohelper.get_fp_addsub(function)
  fp_muldiv                     = asohelper.get_fp_muldiv(function)
  cpiBase                       = function["cpiBase"]
  cpiMem                        = function["cpiMem"]
  cpiBranchPredictor            = function["cpiBranchPredictor"]
  cpiOther                      = nonidle_elapsed_time - cpiBase - cpiMem - cpiBranchPredictor
  fp_instructions               = fp_addsub + fp_muldiv
  non_fp_instructions           = instruction_count - fp_instructions

  #add instructions
  addpd = function["addpd"]     #packed double precision
  addsd = function["addsd"]     #double precision
  addss = function["addss"]     #single precision
  addps = function["addps"]     #packed single precision

  #sub instructions
  subpd = function["subpd"]     #packed double precision
  subsd = function["subsd"]     #double precision
  subss = function["subss"]     #single precision
  subps = function["subps"]     #packed single precision

  #mul instructions
  mulpd = function["mulpd"]     #packed double precision
  mulsd = function["mulsd"]     #double precision
  mulss = function["mulss"]     #single precision
  mulps = function["mulps"]     #packed single precision

  #div instructions
  divpd = function["divpd"]     #packed double precision
  divsd = function["divsd"]     #double precision
  divss = function["divss"]     #single precision
  divps = function["divps"]     #packed single precision

  #check if number of instructions is correct
  assert instruction_count == non_fp_instructions+addpd+addsd+addss+subpd+subsd+subss+subps+mulpd+mulsd+mulss+mulps+divpd+divsd+divss+divps

  if instruction_count > 0:
    #if we only have addsub or muldiv operations
    if fp_addsub == 0 or fp_muldiv == 0:
      n_ports = 1 #only one issue port can be used per fp-instruction
    else:
      n_ports = 2 #the two issue ports can be used simultaneously

    newtime = 0
    #already vectorized and double precision
    #cpibase part used by packed double precision:
    cpiBasePD   = ((addpd + subpd + mulpd + divpd) / instruction_count) *cpiBase
    newtime     += cpiBasePD/n_ports
    #vectorization: SD -> PD
    #cpibase part used by non-vectorized double precision
    cpiBaseSD   = ((addsd + subsd + mulsd + divsd) / instruction_count) *cpiBase
    newtime     += cpiBaseSD / (2*n_ports)
    #already vectorized and single precision
    #cpibase part used by packed single precision:
    cpiBasePS   = ((addps + subps + mulps + divps) / instruction_count) *cpiBase
    newtime     += cpiBasePS / n_ports
    #vectorization: SS -> PS
    #cpibase part used by non-vectorized single precision
    cpiBaseSS   = ((addss + subss + mulss + divss) / instruction_count) *cpiBase
    newtime     += cpiBaseSS / (4*n_ports)
    #these non_fp instructions can't be vectorized
    #cpibase part used by non-floating point instructions:
    cpiBaseNonFP = (non_fp_instructions / instruction_count)            *cpiBase
    newtime     += cpiBaseNonFP
    new_cpiBase = newtime
    #add the branch, mem and other component
    newtime     += cpiMem + cpiBranchPredictor + cpiOther

    #the sum of the cpiBase components should be the total cpiBase component
    total = cpiBasePD + cpiBaseSD + cpiBasePS + cpiBaseSS + cpiBaseNonFP
    assert round(cpiBase) == round(total), "%s != %s" % (round(cpiBase), round(total),)

    time_gain = nonidle_elapsed_time - newtime
    if time_gain > 0:
      #changing properties of function
      optimizedfunction["time_won_back"]        += time_gain
      optimizedfunction["nonidle_elapsed_time"] -= time_gain
      optimizedfunction["cpiBase"]              = new_cpiBase
      optimizedfunction["optimizations"].append(dict(optimization="Vectorization", timegain=time_gain))
      optimizedfunction["originalfunction"]     = function

    assert time_gain >= 0, "we should not gain negative time"

    if fp_instructions == 0:
      #if we don't have any floating point instruction, the time_gain should be zero
      assert time_gain == 0, "%s != %s" % (time_gain, 0,)

  return optimizedfunction

#==================================================================================
#			Memory Plugin
#==================================================================================
def MemoryModule(function):
  optimizedfunction                             = copy.deepcopy(function)
  cpiMem                                        = function["cpiMem"]
  #changing properties of function
  #we remove the memory component
  optimizedfunction["cpiMem"]                   = 0
  optimizedfunction["time_won_back"]            += cpiMem
  optimizedfunction["nonidle_elapsed_time"]     -= cpiMem
  optimizedfunction["bits_used"]                = 0
  optimizedfunction["bits_total"]               = 0
  optimizedfunction["optimizations"].append(dict(optimization="Memory", timegain=cpiMem))
  optimizedfunction["originalfunction"]         = function
  return optimizedfunction

#==================================================================================
#			Thread Level Parallellism
#==================================================================================
#assumptions:
        #parallelization to the number of cores in the simulated system
        #5% of the time cannot be parallelized
def TLPModule(function, config):
  optimizedfunction             = copy.deepcopy(function)
  n_cores                       = float(config['general/total_cores'])
  current_time                  = function["nonidle_elapsed_time"]
  #Amdahl's Law
  new_time                      = ((0.95 * current_time) / n_cores) + 0.05 * current_time
  time_gain                     = current_time - new_time
  if time_gain > 0:
    #change properties of function
    optimizedfunction["time_won_back"]          += time_gain
    optimizedfunction["nonidle_elapsed_time"]   -= time_gain
    optimizedfunction["cpiBase"]                -= time_gain
    optimizedfunction["optimizations"].append(dict(optimization="TLP", timegain=time_gain))
    optimizedfunction["originalfunction"]       = function
  return optimizedfunction

#==================================================================================
#			Memory Level Parallellism
#==================================================================================
#FUTURE WORK
def MLPModule(function, optimizationlist):
  return

#==================================================================================
#			Prefetcher module
#==================================================================================
#FUTURE WORK
def PrefetcherModule():
  return

#==================================================================================
#			Percent Of Data Utilized Plugin
#==================================================================================
def PercentOfDataUtilizedModule(function):
  optimizedfunction             = copy.deepcopy(function)
  bits_used                     = function["bits_used"]
  bits_total                    = function["bits_total"]
  cpiMem                        = function["cpiMem"]
  fractionofdatautilized        = 1
  if bits_total > 0:
    fractionofdatautilized      = bits_used / bits_total
  new_cpiMem    = fractionofdatautilized*cpiMem
  time_gain     = cpiMem - new_cpiMem
  if time_gain > 0:
    #change properties of function
    optimizedfunction["time_won_back"]          += time_gain
    optimizedfunction["nonidle_elapsed_time"]   -= time_gain
    optimizedfunction["bits_total"]             = bits_used
    optimizedfunction["optimizations"].append(dict(optimization="PercentOfDataUtilized", timegain=time_gain, detail={'bits_used':bits_used,'bits_total':bits_total}))
    optimizedfunction["originalfunction"]       = function
  return optimizedfunction



