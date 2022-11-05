// 
// Copyright (C) 2014-2021 Intel Corporation.
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

KNOB<string> KnobProcFile(KNOB_MODE_WRITEONCE, "pintool", "procname", 
    "example", "Name of the procinfo output file");

string get_app_name(int argc, char *argv[])
{
    for(int i=1; i<argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return string(argv[i+1]);
    }

    return (string(""));
}

#if defined(PINPLAY)
VOID fini(INT32 code, VOID *arg)
{
    pinplay_engine->FinalizeProcInfo();
}
#endif

/* ===================================================================== */
int main(int argc, char *argv[])
{
    PIN_InitSymbols();

    sde_pin_init(argc,argv);
    sde_init();

    string exename = get_app_name(argc, argv);
    string procname = KnobProcFile.Value();

#if defined(PINPLAY)

    pinplay_engine = sde_tracing_get_pinplay_engine();
    pinplay_engine->GenerateProcInfo(procname, exename);

    // register Trace to be called to instrument instructions
    PIN_AddFiniFunction(fini, 0);
#endif

    // start program (never returns)
    PIN_StartProgram();

    return 0;
}

