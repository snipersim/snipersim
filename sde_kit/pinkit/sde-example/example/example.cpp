// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#include "pin.H"
extern "C" {
#include "xed-interface.h"
}

#include <iostream>
#include "sde-init.H"

VOID instrument_trace(TRACE trace, VOID *v)
{
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
        {
            xed_extension_enum_t ext;
            ext = (xed_extension_enum_t)INS_Extension(ins);
            /* do whatever Pin intrumentation you want */
            (void)ext; // pacify compiler
        }
    }

}

/* ===================================================================== */
int main(int argc, char *argv[])
{
    PIN_InitSymbols();

    sde_pin_init(argc,argv);
    sde_init();

    // register Trace to be called to instrument instructions
    TRACE_AddInstrumentFunction(instrument_trace, 0);

    // start program (never returns)
    PIN_StartProgram();

    return 0;
}

