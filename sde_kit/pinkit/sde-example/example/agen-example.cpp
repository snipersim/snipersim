// 
// Copyright (C) 2013-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#include "pin.H"
extern "C" {
#include "xed-interface.h"
#include "sde-agen.h"
}
#include <iostream>
#include <fstream>
#include "sde-init.H"

using namespace std;

static KNOB<string> knob_out(KNOB_MODE_WRITEONCE, "pintool", 
                             "oagen", "agen-example.out",
                             "specify output file name");

static UINT64 read_count = 0;
static UINT64 write_count = 0;
static UINT64 agen_count = 0;
static UINT64 ins_count = 0;
static UINT64 agen_icount = 0;

VOID mem_read(THREADID tid, ADDRINT addr, UINT32 size)
{
    read_count += size;
}

VOID mem_write(THREADID tid, ADDRINT addr, UINT32 size)
{
    write_count += size;
}

VOID mem_agen(THREADID tid)
{
    unsigned int i, nrefs = 0;
    if (!sde_agen_init(tid, &nrefs))
        return;

    agen_icount++;
    double v ;
    for (i = 0; i < nrefs; i++)
    {
        sde_memop_info_t meminfo;
        sde_agen_address(tid, i, &meminfo);
        if (meminfo.memop_type == SDE_MEMOP_LOAD)
            mem_read(tid, meminfo.memea, meminfo.bytes_per_ref);
        else
            mem_write(tid, meminfo.memea, meminfo.bytes_per_ref);
        agen_count += meminfo.bytes_per_ref;
        PIN_SafeCopy((void*)&v,(void*)meminfo.memea, meminfo.bytes_per_ref);
        std::cout << v << endl;
    }
}

VOID icount(THREADID tid, UINT32 inss)
{
    ins_count += inss;
}


VOID instrument_trace(TRACE trace, VOID *v)
{
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        INS head = BBL_InsHead(bbl);
        INS_InsertCall(head, IPOINT_BEFORE, (AFUNPTR)icount,
                       IARG_THREAD_ID,
                       IARG_UINT32, BBL_NumIns(bbl),
                       IARG_END);

        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
        {
            xed_decoded_inst_t* xedd  = INS_XedDec(ins);
            sde_bool_t agen_attr = xed_decoded_inst_get_attribute(
                                    xedd, XED_ATTRIBUTE_SPECIAL_AGEN_REQUIRED);

            if (agen_attr)
            {
                if (xed_decoded_inst_get_category(xedd) == XED_CATEGORY_GATHER)
                {
                    /* dump only gather instructions */
                    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)mem_agen,
                                   IARG_THREAD_ID, IARG_END);

                }
                continue;
            }

            //Not an AGEN instruction
            if (INS_IsMemoryRead(ins))
            {
                INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)mem_read,
                               IARG_THREAD_ID,
                               IARG_MEMORYREAD_EA,
                               IARG_MEMORYREAD_SIZE,
                               IARG_END);
            }

            if (INS_HasMemoryRead2(ins))
            {
                INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)mem_read,
                               IARG_THREAD_ID,
                               IARG_MEMORYREAD2_EA,
                               IARG_MEMORYREAD_SIZE,
                               IARG_END);
            }

            if (INS_IsMemoryWrite(ins))
            {
                INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)mem_write,
                               IARG_THREAD_ID,
                               IARG_MEMORYWRITE_EA,
                               IARG_MEMORYWRITE_SIZE,
                               IARG_END);
            }
        }
    }
}


VOID fini(int code, VOID *v)
{
    double read_avg = (double)read_count/ins_count;
    double write_avg = (double)write_count/ins_count;
    double agen_avg = 0;
    std::ofstream out;

    if (agen_icount > 0)
        agen_avg = (double)agen_count/agen_icount;

    out.open(knob_out.Value().c_str());
    out << "Total instructions executed: " << ins_count 
        << " agen instructions: " << agen_icount << endl;
    out << "Total memory accessed: " << endl;
    out << "Read: " << read_count << " bytes " 
        << read_avg << " bytes per instruction " << endl;
    out << "Write: " << write_count << " bytes " 
        << write_avg << " bytes per instruction " << endl;
    out << "Total agen: " << agen_count << " bytes " 
        << agen_avg << " bytes per agen instruction " << endl;
    out.close();
}


/* ===================================================================== */
int main(int argc, char *argv[])
{
    PIN_InitSymbols();

    sde_pin_init(argc,argv);
    sde_init();

    // register Trace to be called to instrument instructions
    TRACE_AddInstrumentFunction(instrument_trace, 0);

    // register fini callback
    PIN_AddFiniFunction(fini, 0);

    // start program (never returns)
    PIN_StartProgram();

    return 0;
}

