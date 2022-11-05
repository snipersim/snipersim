// 
// Copyright (C) 2013-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#include "pin.H"
extern "C" {
#include "xed-interface.h"
}

#include <iostream>
#include "sde-init.H"

#if defined(PINPLAY)
#include "sde-pinplay-supp.H"
#include "pinplay.H"
#include "replayer.H"
static PINPLAY_ENGINE * pinplay_engine;
#endif


VOID instrument_trace(TRACE trace, VOID *v)
{
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
        {
            xed_extension_enum_t ext;
            ext = (xed_extension_enum_t)INS_Extension(ins);
            /* do whatever Pin instrumentation you want */
            (void)ext; // pacify compiler
        }
    }

}

VOID Start(VOID*)
{
    cerr << "PINPLAY starts region" << endl;

    cerr << "Pin OS Pid: " << PIN_GetPid() << endl;
    cerr << "Pinplay OS Pid: " << pinplay_engine->GetOSPid() << endl;

    THREADID tid = PIN_ThreadId();
    string region_basename = pinplay_engine->GetRegionBaseName(tid);
    cerr << "PINPLAY region basename: " << region_basename << endl;
    
}

VOID Stop(VOID*)
{
    cerr << "PINPLAY stops region" << endl;
}

VOID SyncCallback(BOOL enter, THREADID tid, THREADID remote_tid, 
                  UINT64 remote_icount, VOID* arg)
{
    string reason;

    if (enter)
        reason = "entered";
    else
        reason = "left";

    cerr << "Thread: " << tid << " " << reason << " sync waiting for thread " 
         << remote_tid << " to reach icount " << remote_icount << endl;
}

/* ===================================================================== */
int main(int argc, char *argv[])
{
    PIN_InitSymbols();

    sde_pin_init(argc,argv);
    sde_init();
#if defined(PINPLAY)
    pinplay_engine = sde_tracing_get_pinplay_engine();
    
    pinplay_engine->RegisterRegionStart(Start, pinplay_engine);
    pinplay_engine->RegisterRegionStop(Stop,NULL);
    pinplay_engine->RegisterSyncCallback(SyncCallback,NULL);
#endif

    // register Trace to be called to instrument instructions
    TRACE_AddInstrumentFunction(instrument_trace, 0);

    // start program (never returns)
    PIN_StartProgram();

    return 0;
}

