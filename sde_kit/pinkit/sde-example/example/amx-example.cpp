// 
// Copyright (C) 2019-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

/*
 * this pin tool show how to use the API of the reg interface
 * specifically to read and write tiles and tileconfig.
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

#include "sde-reg-interface.H"
#include "sde-init.H"

using namespace std;

/* This example is single threaded, therefore use static state */
static sde_uint32_t palette = 0;
static sde_uint32_t max_names = 0;
static sde_uint32_t cols[SDE_MAX_TILES];
static sde_uint32_t rows[SDE_MAX_TILES];

static const char pad[] = "      ";

//=======================================================
//  Analysis routines
//=======================================================

VOID GetTileCfg(string *str, CONTEXT* ctxt)
{
    xed_reg_enum_t reg = XED_REG_TILECONFIG;
    sde_uint8_t buff[SDE_BYTES_PER_TILECFG];
    sde_get_register(ctxt, 0, reg, buff, SDE_BYTES_PER_TILECFG);

    /* TileCFG is a buffer of 64 bytes. The interpretation of the bytes to
     * fields can be seen from the following code */

    /* update the global (static) state */
    palette = buff[0];
    if (palette == 0)
        max_names = 0;
    else if (palette == 1)
        max_names = SDE_MAX_TILES;
    else
        max_names = 0;

    for (int i=0; i<SDE_MAX_TILES; i++) {
        cols[i] = ((sde_uint16_t*)(buff+16))[i];
    }
    for (int i=0; i<SDE_MAX_TILES; i++) {
        rows[i] = ((sde_uint8_t*)(buff+48))[i];
    }

    sde_addr_t ip = PIN_GetContextReg(ctxt, REG_INST_PTR);
    cout << "INS: " << hex << "0x" << ip << dec << ":  ";
    cout << (*str) << endl;
    cout << pad << "Palette: " << (unsigned int)buff[0] << endl;
    cout << pad << "Start row: " << (unsigned int)buff[1] << endl;
    for (sde_uint32_t i=0; i<max_names; i++) {
        cout << pad << "Tile[" << i << "]: " << cols[i] << " " << rows[i] << endl;
    }

    cout << endl;
}

VOID GetTile(string *str, CONTEXT* ctxt, sde_uint32_t tile_num)
{
    xed_reg_enum_t reg = static_cast<xed_reg_enum_t>(XED_REG_TMM0 + tile_num);
    sde_uint8_t buff[SDE_BYTES_PER_TILE];
    sde_get_register(ctxt, 0, reg, buff, SDE_BYTES_PER_TILE);

    sde_addr_t ip = PIN_GetContextReg(ctxt, REG_INST_PTR);
    cout << "INS: " << hex << "0x" << ip << dec << ":  ";
    cout << (*str) << endl;
    cout << "Tile[" << tile_num << "]: (" 
         << rows[tile_num] << "," << cols[tile_num] << ") ---- " ;

    /* Tile is a buffer of SDE_BYTES_PER_TILE (1024) bytes regardless of 
     * the cols/rows defined in the tile configuration. 
     * Each row is SDE_BYTES_PER_TILEROW (64) bytes.
     * The number of bytes inside the row are taken from the columns bytes 
     * field in the configuration */
    for (unsigned int i=0; i<rows[tile_num]; i++) {
        sde_uint8_t *row_bytes = buff + i * SDE_BYTES_PER_TILEROW;
        for (unsigned int j=0; j<cols[tile_num]; j++) {
            if ((j%16) == 0) {
                if (j==0) {
                    cout << endl << "[" << setw(2) << i << "]: ";
                }
                else {
                    cout << endl << pad;
                }
            }
            unsigned int val = row_bytes[j];
            cout << setw(4) << val;
        }
    }
    cout << endl << endl;
}

// Pin calls this function every time a new instruction is encountered
VOID Instruction(INS ins, VOID *v)
{
    // Find the instructions that move a value from memory to a register
    if (INS_Opcode(ins) == XED_ICLASS_LDTILECFG ||
        INS_Opcode(ins) == XED_ICLASS_TILERELEASE)
    {
        string *str = new string(INS_Disassemble(ins));
        INS_InsertCall(ins,
                       IPOINT_AFTER,
                       AFUNPTR(GetTileCfg),
                       IARG_PTR, str,
                       IARG_CONST_CONTEXT,
                       IARG_END);
        return;
    }

    if (INS_Category(ins) == XED_CATEGORY_AMX_TILE)
    {
        string *str = new string(INS_Disassemble(ins));
        xed_decoded_inst_t* xedd = INS_XedDec(ins);
        xed_reg_enum_t xedreg = xed_decoded_inst_get_reg(xedd, XED_OPERAND_REG0);
        sde_uint32_t tile_num = xedreg - XED_REG_TMM0;
        INS_InsertCall(ins,
                       IPOINT_AFTER,
                       AFUNPTR(GetTile),
                       IARG_PTR, str,
                       IARG_CONST_CONTEXT,
                       IARG_UINT32, tile_num,
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

