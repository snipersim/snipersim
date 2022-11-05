// 
// Copyright (C) 2017-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

/*
  This file creates a PinPlay base tool with the capability to gather loop
  statistics during replay.
*/

#include "dcfg_pin_api.H"
#include "pinplay.H"
#include "loop-profiler.H"
#if defined(SDE_INIT)
#include "sde-init.H"
#endif
#if defined(PINPLAY)
#include "sde-pinplay-supp.H"
#include "pinplay.H"
#include "replayer.H"
static PINPLAY_ENGINE * pinplay_engine;
#endif

using namespace dcfg_pin_api;

loop_profiler::LOOP_PROFILER loopProfiler;

int main(int argc, char* argv[])
{
#if defined(SDE_INIT)
    sde_pin_init(argc,argv);
    sde_init();
#else
    if(PIN_Init(argc,argv))
    {
        cerr << "This tool creates a Dynamic Control Flow Graph (DCFG) of "
                "the target application.\n\n";
        cerr << KNOB_BASE::StringKnobSummary() << endl;
        return -1;
    }
#endif

    pinplay_engine = sde_tracing_get_pinplay_engine();

    // Activate DCFG generation if enabling knob was used.
    DCFG_PIN_MANAGER* dcfgMgr = DCFG_PIN_MANAGER::new_manager();
    if (dcfgMgr->dcfg_enable_knob()) {
        dcfgMgr->activate(pinplay_engine);
    }

    // Activate loop tracking.
    loopProfiler.activate();
    
    PIN_StartProgram();    // Never returns
    return 0;
}
