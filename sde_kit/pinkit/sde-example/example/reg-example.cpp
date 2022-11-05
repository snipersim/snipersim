// 
// Copyright (C) 2014-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

/*
 * this pin tool show how to use the API of the reg interface
 * the pin tool assume the App to be tested is wrote to zmm1 register
 * with the value of the expected_value_match variable
 * in Instruction we find the instruction that use the relevant register
 * we plant the function DoLoad after this instruction
 * in DoLoad we retrieve it is value using sde_get_register
 * and then we compare it is value to the expected value we used in the app
 * finally we print if the value matched or not
 *
 * testing done:
 * run the pin tool with different values in the registers
 *
 */
#include "pin.H"
extern "C" {
#include "xed-interface.h"
}
#include <iostream>
#include <stdio.h>
#include "sde-init.H"
#include "sde-reg-interface.H"

using namespace std;

//=======================================================
//  Analysis routines
//=======================================================

VOID DoLoad(CONTEXT* ctxt)
{
    xed_reg_enum_t reg = XED_REG_ZMM1;
    sde_uint_t buf_bytes = 512;
    sde_uint64_t zmm[8];
    sde_get_register(ctxt, 0, reg, (sde_uint8_t*)zmm, buf_bytes);
    int i;
    for(i=0;i<8;i++) {
        std::cout << zmm[i] << endl;
    }
}

// Pin calls this function every time a new instruction is encountered
VOID Instruction(INS ins, VOID *v)
{
    // Check if the instruction is in the main executable
    ADDRINT addr = INS_Address(ins);
    IMG img = IMG_FindByAddress(addr);
    if (!IMG_Valid(img) || !IMG_IsMainExecutable(img))
        return;

    // Find the instructions that move a value from memory to a register
    if (INS_Opcode(ins) == XED_ICLASS_VMOVDQU64)
    {
        // op0 <- *op1
        INS_InsertCall(ins,
                       IPOINT_AFTER,
                       AFUNPTR(DoLoad),
                       IARG_CONST_CONTEXT,
                       IARG_END);
    }
}

/* ===================================================================== */
int main(int argc, char *argv[])
{
    PIN_InitSymbols();

    sde_pin_init(argc,argv);
    sde_init();

    INS_AddInstrumentFunction(Instruction, 0);

    // start program (never returns)
    PIN_StartProgram();

    return 0;
}

