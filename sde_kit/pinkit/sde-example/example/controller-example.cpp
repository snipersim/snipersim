// 
// Copyright (C) 2013-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#include <iostream>
#include <string>
#include <sstream>

#include "pin.H"
#include "instlib.H"
#include "sde-init.H"
#include "ialarm.H"
#include "atomic.hpp"

// sde-control.H will allow to use the SDE's API
// for requesting a pointer to the SDE's controller
#include "sde-control.H"
#include "pcregions_control.H"

// This tool demonstrates how to register a handler for various
// events reported by SDE's controller module.
// At every event, global (all threads) instruction count is
// printed along with the triggering thread number and PC.

// Optional feature:
// Monitor occurrences of up to three PCs. These PC can be provided
// using the three Knob*PC knobs below.
// A global (all threads) count of the monitored PCs is output on
// certain events.

#if defined(PINPLAY)
#include "sde-pinplay-supp.H"
using namespace INSTLIB;
#endif

KNOB<UINT32> KnobControlThread(KNOB_MODE_WRITEONCE, "pintool", "control_tid",
         "0", "Controlling thread for incoming ROI");
KNOB<UINT64> KnobWStartPC(KNOB_MODE_WRITEONCE, "pintool", "wstart_pc",
         "0", "Warmup start PC");
KNOB<UINT64> KnobSStartPC(KNOB_MODE_WRITEONCE, "pintool", "sstart_pc",
         "0", "Simulation start PC");
KNOB<UINT64> KnobSEndPC(KNOB_MODE_WRITEONCE, "pintool", "send_pc",
         "0", "Simulation end PC");


using namespace CONTROLLER;

typedef enum {WSTART, SSTART, SEND} PCTYPE;

static CACHELINE_COUNTER global_addr_counter[SEND+1] = {0,0,0,0,0,0};
static CACHELINE_COUNTER tid_addr_counter[SEND+1] = {0,0,0,0,0,0};
static CACHELINE_COUNTER global_ins_counter  = {0,0};

static UINT32 control_tid = 0;
static UINT64 wstart_pc = 0;
static UINT64 sstart_pc = 0;
static UINT64 send_pc = 0;
static BOOL print_roi_info = FALSE;

static PIN_LOCK output_lock;

// Contains knobs and instrumentation to recognize start/stop points
static CONTROLLER::CONTROL_MANAGER *sde_control = SDE_CONTROLLER::sde_controller_get();

// Create pc regions object
KNOB_COMMENT pcregion_knob_family("pintool:pcregions_control", "PC regions control knobs");
CONTROL_ARGS args("","pintool:pcregions_control");
CONTROL_PCREGIONS pcregions(args,sde_control);

VOID Handler(EVENT_TYPE ev, VOID * v, CONTEXT * ctxt, VOID * ip,
             THREADID tid, BOOL bcast)
{
    PIN_GetLock(&output_lock, tid+1);

    string eventstr;

    switch(ev)
    {
      case EVENT_START:
        eventstr="Sim-Start";
        break;

      case EVENT_WARMUP_START:
        eventstr="Warmup-Start";
        break;

      case EVENT_STOP:
        eventstr="Sim-End";
        break;

     case EVENT_WARMUP_STOP:
        eventstr="Warmup-Stop";
        break;

      case EVENT_THREADID:
        std::cerr << "ThreadID" << endl;
        eventstr="ThreadID";
        break;

      default:
        ASSERTX(false);
        break;
    }

    if (print_roi_info) {
        for (UINT32 roi_kind = WSTART; roi_kind <= SEND; roi_kind++) {
            if (roi_kind == WSTART)
                std::cerr <<  "ROI " << eventstr << " WSTART_PC 0x" << hex << wstart_pc;
            if (roi_kind == SSTART)
                std::cerr <<  "ROI " << eventstr << " SSTART_PC 0x" << hex << sstart_pc;
            if (roi_kind == SEND)
                std::cerr <<  "ROI " << eventstr << " SEND_PC 0x" << hex << send_pc;

            std::cerr << " global_count " << dec << global_addr_counter[roi_kind]._count;
            std::cerr << " tid" << dec << control_tid << "_count "
                      << tid_addr_counter[roi_kind]._count;
            std::cerr << " global_ins_count " << dec << global_ins_counter._count << endl;
        }
    }
    else {
        std::cerr << eventstr;
        std::cerr << " tid " << dec << tid << " pc " << hex << ip;
        std::cerr << " global_ins_count " << dec << global_ins_counter._count << endl;
    }

    PIN_ReleaseLock(&output_lock);
}

// increment counter for the PCTYPE 'pct'
VOID Countaddr(UINT32 pct, THREADID tid)
{
    ATOMIC::OPS::Increment<UINT64>(&global_addr_counter[pct]._count,1);
    if (tid == control_tid)
        tid_addr_counter[pct]._count++;
}

// Is called for every instruction and instruments reads and writes
VOID Instruction(INS ins, VOID *v)
{
    if (INS_Address(ins) == wstart_pc) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)Countaddr,
                       IARG_UINT32, WSTART,
                       IARG_THREAD_ID,
                       IARG_END);
    }

    if (INS_Address(ins) == sstart_pc) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)Countaddr,
                       IARG_UINT32, SSTART,
                       IARG_THREAD_ID,
                       IARG_END);
    }

    if (INS_Address(ins) == send_pc) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)Countaddr,
                       IARG_UINT32, SEND,
                       IARG_THREAD_ID,
                       IARG_END);
    }
}

// This function is called before every block
// Use the fast linkage for calls
VOID PIN_FAST_ANALYSIS_CALL docount(ADDRINT c)
{
    ATOMIC::OPS::Increment<UINT64>(&global_ins_counter._count, c);
}

// Pin calls this function every time a new basic block is encountered
// It inserts a call to docount
VOID Trace(TRACE trace, VOID *v)
{
    // Visit every basic block  in the trace
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        // Insert a call to docount for every bbl, passing
        // the number of instructions.
        // IPOINT_ANYWHERE allows Pin to schedule the call
        //  anywhere in the bbl
        // to obtain best performance.
        // Use a fast linkage for the call.
        BBL_InsertCall(bbl, IPOINT_ANYWHERE, AFUNPTR(docount),
                       IARG_FAST_ANALYSIS_CALL,
                       IARG_UINT32, BBL_NumIns(bbl),
                       IARG_END);
    }
}

// argc, argv are the entire command line, including pin -t <toolname> -- ...
int main(int argc, char * argv[])
{
    sde_pin_init(argc,argv);
    PIN_InitLock(&output_lock);

    INS_AddInstrumentFunction(Instruction, 0);
    TRACE_AddInstrumentFunction(Trace, 0);

    //Register handler on SDE's controller, must be done before PIN_StartProgram
    sde_control->RegisterHandler(Handler, 0, 0);

    sde_init();

    control_tid = KnobControlThread.Value();
    wstart_pc = KnobWStartPC.Value();
    sstart_pc = KnobSStartPC.Value();
    send_pc = KnobSEndPC.Value();

    if(wstart_pc || sstart_pc || send_pc)
        print_roi_info = TRUE;

    pcregions.Activate();

    // Start the program, never returns
    PIN_StartProgram();

    return 0;
}
