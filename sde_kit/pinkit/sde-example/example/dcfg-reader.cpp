// 
// Copyright (C) 2017-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

// Simple example program to read a DCFG and print some statistics.

#include "dcfg_api.H"
#include "dcfg_trace_api.H"

#include <stdlib.h>
#include <assert.h>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>

using namespace std;
using namespace dcfg_api;
using namespace dcfg_trace_api;

char * inner_loops_file = NULL;
char * source_loops_file = NULL;
char * dcfg_file = NULL;
char * edge_file = NULL;

// Class to collect and print some simple statistics.
class Stats {
    UINT64 _count, _sum, _max, _min;

public:
    Stats() : _count(0), _sum(0), _max(0), _min(0) { }

    void addVal(UINT64 val, UINT64 num=1) {
        _sum += val;
        if (!_count || val > _max)
            _max = val;
        if (!_count || val < _min)
            _min = val;
        _count += num;
    }

    UINT64 getCount() const {
        return _count;
    }

    UINT64 getSum() const {
        return _sum;
    }

    float getAve() const {
        return _count ? (float(_sum) / _count) : 0.f;
    }

    void print(int indent, string valueName, string containerName) {
        for (int i = 0; i < indent; i++)
            cout << " ";
        cout << "Num " << valueName << " , " << getSum();
        if (_count)
            cout << ", ave " << valueName << "/" <<
                containerName << " , " << getAve() <<
                " max , " << _max << ", min , " << _min ;
        cout << endl;
    }
};

// Summarize DCFG contents.
void summarizeDcfg(DCFG_DATA_CPTR dcfg) {

    // output averages to 2 decimal places.
    cout << setprecision(2) << fixed;

    cout << "Summary of DCFG, " << dcfg_file << endl;

    // processes.
    DCFG_ID_VECTOR proc_ids;
    dcfg->get_process_ids(proc_ids);
    cout << " Num processes           = " << proc_ids.size() << endl;
    for (size_t pi = 0; pi < proc_ids.size(); pi++) {
        DCFG_ID pid = proc_ids[pi];
        ofstream os; // inner loops stats
        ofstream sos; // source-level loops stats

        // Get info for this process.
        DCFG_PROCESS_CPTR pinfo = dcfg->get_process_info(pid);
        assert(pinfo);
        UINT32 numThreads = pinfo->get_highest_thread_id() + 1;

        cout << "  Num threads , " << numThreads << endl;
        cout << "  Instr count , " << pinfo->get_instr_count() << endl;
        if (numThreads > 1) {
            for (UINT32 t = 0; t < numThreads; t++)
                cout << "  Instr count on thread " << t <<
                    " , " << pinfo->get_instr_count_for_thread(t) << endl;
        }

        // Edge IDs.
        DCFG_ID_SET edge_ids;
        pinfo->get_internal_edge_ids(edge_ids);
        cout << "  Num edges   , " << edge_ids.size() << endl;

        // Overall stats.
        Stats bbStats, bbSizeStats, bbCountStats, bbInstrCountStats,
            routineStats, routineCallStats, loopStats, loopTripStats;

        // Images.
        DCFG_ID_VECTOR image_ids;
        pinfo->get_image_ids(image_ids);
        cout << "  Num images  , " << image_ids.size() << endl;
        if(source_loops_file)
        {
            sos.open(source_loops_file, ios_base::out);
            if (!sos.is_open()) {
                cerr << "Error: cannot open '" 
                    << source_loops_file <<
                    "' for saving inner source loop statistics." 
                    << endl;
                return;
            }
            sos << "loopid,filename:linenumber,#visits, #iterations,#static-instrs,#dynamic-instructions" << endl;
        }

        for (size_t ii = 0; ii < image_ids.size(); ii++) {
            DCFG_IMAGE_CPTR iinfo = pinfo->get_image_info(image_ids[ii]);
            assert(iinfo);

            // Basic block, routine and loop IDs for this image.
            DCFG_ID_VECTOR bb_ids, routine_ids, loop_ids, inner_loop_ids;
            iinfo->get_basic_block_ids(bb_ids);
            iinfo->get_routine_ids(routine_ids);
            iinfo->get_loop_ids(loop_ids);
            iinfo->get_loop_ids(inner_loop_ids);

            cout << "  Image , " << image_ids[ii] << endl;
            cout << "   Load addr        , 0x" << hex << iinfo->get_base_address() << dec << endl;
            cout << "   Size             , " << iinfo->get_size() << endl;
            cout << "   File             , '" << *iinfo->get_filename() << "'" << endl;
            cout << "   Num basic blocks , " << bb_ids.size() << endl;
            cout << "   Num routines     , " << routine_ids.size() << endl;
            cout << "   Num loops        , " << loop_ids.size() << endl;

            // Basic blocks.
            bbStats.addVal(bb_ids.size());
            for (size_t bi = 0; bi < bb_ids.size(); bi++) {
                if (pinfo->is_special_node(bb_ids[bi]))
                    continue;
                DCFG_BASIC_BLOCK_CPTR bbinfo = pinfo->get_basic_block_info(bb_ids[bi]);
                assert(bbinfo);

                bbSizeStats.addVal(bbinfo->get_num_instrs());
                bbCountStats.addVal(bbinfo->get_exec_count());
                bbInstrCountStats.addVal(bbinfo->get_instr_count(), bbinfo->get_exec_count());
            }
                
            // Routines.
            routineStats.addVal(routine_ids.size());
            for (size_t ri = 0; ri < routine_ids.size(); ri++) {
                DCFG_ROUTINE_CPTR rinfo = iinfo->get_routine_info(routine_ids[ri]);
                assert(rinfo);
                routineCallStats.addVal(rinfo->get_entry_count());
            }

            // Loops.
            loopStats.addVal(loop_ids.size());
            unsigned int no_source_loops = 0;
            for (size_t li = 0; li < loop_ids.size(); li++) {
                DCFG_LOOP_CPTR linfo = iinfo->get_loop_info(loop_ids[li]);
                assert(linfo);
                if(linfo->get_parent_loop_id())
                {
                    // Any loop that is a parent cannot be an inner loop
                    inner_loop_ids.erase(std::remove(inner_loop_ids.begin(), 
                        inner_loop_ids.end(), 
                        linfo->get_parent_loop_id()), inner_loop_ids.end());
                }
                loopTripStats.addVal(linfo->get_iteration_count());
                if (source_loops_file)
                {
                    DCFG_ID loopId = loop_ids[li];
                    DCFG_ID_VECTOR loopBbs;
                    DCFG_BASIC_BLOCK_CPTR loopIdData = pinfo->get_basic_block_info(loopId);
                    DCFG_ID_VECTOR entryEdgeIds;
                    linfo->get_entry_edge_ids(entryEdgeIds);
                    linfo->get_basic_block_ids(loopBbs);
                    if(loopIdData->get_source_filename() && loopIdData->get_exec_count())
                    {
                        no_source_loops++;
                        UINT64 num_instrs = 0;
                        UINT64 num_dynamic_instrs = 0;
                        UINT64 num_visits = 0;
                        for (size_t bi = 0; bi < loopBbs.size(); bi++) {
                            DCFG_ID bbId = loopBbs[bi];
                            DCFG_BASIC_BLOCK_CPTR bbData = pinfo->get_basic_block_info(bbId);
                            num_instrs += bbData->get_num_instrs();
                            num_dynamic_instrs += bbData->get_instr_count();
                        }
                        for (size_t ei = 0; ei < entryEdgeIds.size(); ei++) {
                                DCFG_ID entryEdgeId = entryEdgeIds[ei];
                                DCFG_EDGE_CPTR entryEdgeData = pinfo->get_edge_info(entryEdgeId);
                                num_visits += entryEdgeData->get_exec_count();
                        }
                        if(num_visits)
                            sos << dec << loopId  << "," << *(loopIdData->get_source_filename()) << ":" << loopIdData->get_source_line_number() << "," << num_visits << "," << loopIdData->get_exec_count() << "," << num_instrs << "," << num_dynamic_instrs << endl;
                        else
                            sos << dec << loopId  << "," << *(loopIdData->get_source_filename()) << ":" << loopIdData->get_source_line_number() << "," << "*NA*" << "," << loopIdData->get_exec_count() << "," << num_instrs <<  "," << num_dynamic_instrs << endl;
                    }
                }
            }
            cout << "   Num inner-most loops        , " << inner_loop_ids.size() << endl;
            cout << "   Num source loops        , " << no_source_loops << endl;
            if (inner_loops_file)
            {
                bool first = true;
                os.open(inner_loops_file, ios_base::out);
                if (!os.is_open()) {
                    cerr << "Error: cannot open '" << inner_loops_file <<
                        "' for saving inner source loop statistics." << endl;
                return;
                }

                for (size_t li = 0; li < inner_loop_ids.size(); li++) {
                    DCFG_LOOP_CPTR linfo = iinfo->get_loop_info(inner_loop_ids[li]);
                    assert(linfo);
                    DCFG_ID loopId = inner_loop_ids[li];
                    DCFG_ID_VECTOR loopBbs;
                    DCFG_BASIC_BLOCK_CPTR loopIdData = pinfo->get_basic_block_info(loopId);
                    DCFG_ID_VECTOR entryEdgeIds;
                    linfo->get_entry_edge_ids(entryEdgeIds);
                    linfo->get_basic_block_ids(loopBbs);
                    if(loopIdData->get_source_filename() && loopIdData->get_exec_count())
                    {
                        UINT64 num_instrs = 0;
                        UINT64 num_dynamic_instrs = 0;
                        UINT64 num_visits = 0;
                        for (size_t bi = 0; bi < loopBbs.size(); bi++) {
                            DCFG_ID bbId = loopBbs[bi];
                            DCFG_BASIC_BLOCK_CPTR bbData = pinfo->get_basic_block_info(bbId);
                            num_instrs += bbData->get_num_instrs();
                            num_dynamic_instrs += bbData->get_instr_count();
                        }
                        for (size_t ei = 0; ei < entryEdgeIds.size(); ei++) {
                                DCFG_ID entryEdgeId = entryEdgeIds[ei];
                                DCFG_EDGE_CPTR entryEdgeData = pinfo->get_edge_info(entryEdgeId);
                                num_visits += entryEdgeData->get_exec_count();
                        }
                        UINT32 nesting=0;
                        DCFG_ID parent_id = linfo->get_parent_loop_id();
                        while (parent_id)
                        {
                            DCFG_LOOP_CPTR plinfo = iinfo->get_loop_info(parent_id);
                            nesting++;
                            parent_id = plinfo->get_parent_loop_id();
                        }

                        if(first)
                        {
                            first = false;
                            os << "nesting-depth,filename:linenumber,#visits, #iterations,#static-instrs,#dynamic-instructions" << endl;
                        }
                        parent_id = loopId;
                        while (parent_id)
                        {
                            DCFG_LOOP_CPTR plinfo = iinfo->get_loop_info(parent_id);
                            DCFG_BASIC_BLOCK_CPTR ploopIdData = pinfo->get_basic_block_info(parent_id);
                            DCFG_ID_VECTOR loopBbs;
                            DCFG_ID_VECTOR entryEdgeIds;
                            plinfo->get_entry_edge_ids(entryEdgeIds);
                            plinfo->get_basic_block_ids(loopBbs);
                            if(ploopIdData->get_source_filename() && ploopIdData->get_exec_count())
                            {
                                UINT64 num_instrs = 0;
                                UINT64 num_dynamic_instrs = 0;
                                UINT64 num_visits = 0;
                                for (size_t bi = 0; bi < loopBbs.size(); bi++) {
                                    DCFG_ID bbId = loopBbs[bi];
                                    DCFG_BASIC_BLOCK_CPTR bbData = pinfo->get_basic_block_info(bbId);
                                    num_instrs += bbData->get_num_instrs();
                                    num_dynamic_instrs += bbData->get_instr_count();
                                }
                                for (size_t ei = 0; ei < entryEdgeIds.size(); ei++) {
                                        DCFG_ID entryEdgeId = entryEdgeIds[ei];
                                        DCFG_EDGE_CPTR entryEdgeData = pinfo->get_edge_info(entryEdgeId);
                                        num_visits += entryEdgeData->get_exec_count();
                                }
                                os << dec << nesting  << "," << *(ploopIdData->get_source_filename()) << ":" << ploopIdData->get_source_line_number() << "," <<  num_visits << "," << ploopIdData->get_exec_count() << "," << num_instrs << "," << num_dynamic_instrs << endl;
                            }
                            nesting--;
                            parent_id = plinfo->get_parent_loop_id();
                        }
                        os << endl;
                    }
                }
            }
        }

        if(source_loops_file) sos.close();
        if(inner_loops_file) os.close();
        routineStats.print(2, "routines", "image");
        routineCallStats.print(2, "routine calls", "routine");
        loopStats.print(2, "loops", "image");
        loopTripStats.print(2, "loop iterations", "loop");
        bbStats.print(2, "basic blocks", "image");
        bbSizeStats.print(2, "static instrs", "basic block");
        bbCountStats.print(2, "basic-block executions", "basic block");
        bbInstrCountStats.print(2, "dynamic instrs", "basic block execution");
    }
}

// Summarize DCFG trace contents.
void summarizeTrace(DCFG_DATA_CPTR dcfg, string tracefile) {

    // processes.
    DCFG_ID_VECTOR proc_ids;
    dcfg->get_process_ids(proc_ids);
    for (size_t pi = 0; pi < proc_ids.size(); pi++) {
        DCFG_ID pid = proc_ids[pi];

        // Get info for this process.
        DCFG_PROCESS_CPTR pinfo = dcfg->get_process_info(pid);
        assert(pinfo);

        // Make a new reader.
        DCFG_TRACE_READER* traceReader = DCFG_TRACE_READER::new_reader(pid);

        // threads.
        for (UINT32 tid = 0; tid <= pinfo->get_highest_thread_id(); tid++) {

            // Open file.
            cerr << "Reading DCFG trace for PID " << pid <<
                " and TID " << tid << " from '" << tracefile << "'..." << endl;
            string errMsg;
            if (!traceReader->open(tracefile, tid, errMsg)) {
                cerr << "error: " << errMsg << endl;
                delete traceReader;
                return;
            }

            // Header.
            cout << "edge id,basic-block id,basic-block addr,basic-block symbol,num instrs in BB" << endl;

            // Read until done.
            size_t nRead = 0;
            bool done = false;
            DCFG_ID_VECTOR edge_ids;
            while (!done) {

                if (!traceReader->get_edge_ids(edge_ids, done, errMsg))
                {
                    cerr << "error: " << errMsg << endl;
                    done = true;
                }
                nRead += edge_ids.size();
                for (size_t j = 0; j < edge_ids.size(); j++) {
                    DCFG_ID edgeId = edge_ids[j];

                    // Get edge.
                    DCFG_EDGE_CPTR edge = pinfo->get_edge_info(edgeId);
                    if (!edge) continue;
                    if (edge->is_exit_edge_type()) {
                        cout << edgeId << ",end" << endl;
                        continue;
                    }

                    // Get BB at target.
                    DCFG_ID bbId = edge->get_target_node_id();
                    DCFG_BASIC_BLOCK_CPTR bb = pinfo->get_basic_block_info(bbId);
                    if (!bb) continue;
                    const string* symbol = bb->get_symbol_name();
                    
                    // print info.
                    cout << edgeId << ',' << bbId << ',' <<
                        (void*)bb->get_first_instr_addr() << ',' <<
                        '"' << (symbol ? *symbol : "unknown") << '"' << ',' <<
                        bb->get_num_instrs() << endl;
                }
                edge_ids.clear();
            }
            cerr << "Done reading " << nRead << " edges." << endl;
        }
        delete traceReader;
    }
}

// Print usage and exit.
void usage(const char* cmd) {
    cerr << "This program inputs a DCFG file in JSON format and outputs summary data and statistics." << endl <<
        "It optionally inputs a DCFG-Trace file and outputs a sequence of edges." << endl <<
        "It optionally prints out stats, to a specified file, \n \t about inner loops with source file / line number information." << endl <<
        "Usage:" << endl <<
        cmd << " [ -inner_source_loops_stats <stats-file> -all_source_loops_stats <stts-file> ] <dcfg-file> [<dcfg-trace-file>]" << endl;
    exit(1);
}

void  parse_args(int argc, char *argv[])
{
    for(int i = 1; i < argc;) // skip argv[0], the program name
    {
        if(string("-inner_source_loops_stats") ==  string(argv[i])) 
        {
            if((i+1)== argc)
            {
                cerr << "Must provide a stats-filename after '-inner_loops_stats'." << endl;
                usage(argv[0]);
            }
            else
            {
                inner_loops_file = argv[i+1];
                i = i + 2; // we have consumed two args '-inner_loops_stats' and '<stats-filename>'
            }
        }
        else if(string("-all_source_loops_stats") ==  string(argv[i])) 
        {
            if((i+1)== argc)
            {
                cerr << "Must provide a stats-filename after '-source_loops_stats'." << endl;
                usage(argv[0]);
            }
            else
            {
                source_loops_file = argv[i+1];
                i = i + 2; // we have consumed two args '-source_loops_stats' and '<source-loops-filename>'
            }
        }
        else
        {
            if(!dcfg_file) 
            {
              dcfg_file = argv[i]; // first un-used arg is dcfg_filename
              i++;
            }
            else if(!edge_file) 
            {
                edge_file = argv[i]; // second un-used arg is edge_filename
                i++;
            }
            else
            {
                cerr << "Unused argument " << argv[i] <<  endl;
                usage(argv[0]);
            }
        }
    }
}

int main(int argc, char* argv[]) {

    if (argc < 2)
        usage(argv[0]);

    parse_args(argc, argv);

    if (!dcfg_file)
    {
        cerr << "Missing dcfg file. " << endl;
        usage(argv[0]);
    }
    // Make a new DCFG object.
    DCFG_DATA* dcfg = DCFG_DATA::new_dcfg();


    // Read from file.
    cerr << "Reading DCFG from '" << dcfg_file << "'..." << endl;
    string errMsg;
    if (!dcfg->read(dcfg_file, errMsg)) {
        cerr << "error: " << errMsg << endl;
        delete dcfg;
        return 1;
    }
    
    // write some summary data from DCFG.
    summarizeDcfg(dcfg);

    // Second argument should be a DCFG-trace file.
    if (edge_file) {
        summarizeTrace(dcfg, edge_file);        
    }

    // free memory.
    delete dcfg;

    return 0;
}
