// 
// Copyright (C) 2017-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

/*
  This file creates a PinPlay driver with the capability to gather BBVs
  using DCFG+replay.
*/

#include "dcfg_pin_api.H"
#include "pinplay.H"
#include "looppoint.H"
#if defined(SDE_INIT)
#include "sde-init.H"
#endif
#if defined(PINPLAY)
#if defined(SDE_INIT)
#include "sde-pinplay-supp.H"
#endif
#include "pinplay.H"
static PINPLAY_ENGINE * pinplay_engine;
#endif

using namespace dcfg_pin_api;

static ISIMPOINT * isimpoint;
looppoint::LOOPPOINT loopPoint;

int main(int argc, char* argv[])
{
#if defined(SDE_INIT)
    sde_pin_init(argc,argv);
    sde_init();
#else
    if(PIN_Init(argc,argv))
    {
        cerr << "This tool creates BBV profile based on "
                "Dynamic Control Flow Graph (DCFG) of "
                "input pinball.\n\n";
        cerr << KNOB_BASE::StringKnobSummary() << endl;
        return -1;
    }
#endif
    PIN_InitSymbols();

#if defined(SDE_INIT)
    // This is a replay-only tool (for now)
    pinplay_engine = sde_tracing_get_pinplay_engine();
    isimpoint = sde_tracing_get_isimpoint();
#endif

    // Activate DCFG generation if enabling knob was used.
    DCFG_PIN_MANAGER* dcfgMgr = DCFG_PIN_MANAGER::new_manager();
    if (dcfgMgr->dcfg_enable_knob()) {
        dcfgMgr->activate(pinplay_engine);
    }

    // Activate loop profiling.
    loopPoint.activate(isimpoint);
    
    PIN_StartProgram();    // Never returns
    delete dcfgMgr;
    return 0;
}
