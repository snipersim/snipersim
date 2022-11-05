// 
// Copyright (C) 2019-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#include <iostream>
#include <string>
#include <sstream>

#include "pin.H"
#include "sde-init.H"
#include "ialarm.H"
#include "pcregions_control.H"

// sde-control.H will allow to use the SDE's API
// for requesting a pointer to the SDE's controller
#include "sde-control.H"

#if defined(PINPLAY)
#include "sde-pinplay-supp.H"
using namespace INSTLIB;
#endif

// Controller from SDE
using namespace CONTROLLER;
static CONTROLLER::CONTROL_MANAGER *sde_control = SDE_CONTROLLER::sde_controller_get();

// Create pc regions object
KNOB_COMMENT pcregion_knob_family("pintool:pcregions_control", "PC regions control knobs");
CONTROL_ARGS args("","pintool:pcregions_control");
CONTROL_PCREGIONS pcregions(args,sde_control);

// argc, argv are the entire command line, including pin -t <toolname> -- ...
int main(int argc, char * argv[])
{
    sde_pin_init(argc,argv);

    PIN_InitSymbols();

    pcregions.Activate();

    sde_init();

    // Start the program, never returns
    PIN_StartProgram();

    return 0;
}
