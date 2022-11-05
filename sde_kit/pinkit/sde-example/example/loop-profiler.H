// 
// Copyright (C) 2017-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

/*
 The LOOP_PROFILER class defined in this file provides functionality for a
 PinPlay tool that tracks some statistics within loops defined by
 inputting a DCFG data file.

 It can be used as a starting point for other tools that
 need to track DCFG basic blocks, edges, and/or loops.
*/

#ifndef LOOP_PROFILER_H
#define LOOP_PROFILER_H

#include "dcfg_pin_api.H"
#include "pinplay.H"

#include <iomanip>

#if !defined (TARGET_WINDOWS)
#  include <sys/cdefs.h>
#endif
#include <unordered_map>

using namespace std;
using namespace tr1;
using namespace dcfg_api;
using namespace dcfg_pin_api;

// buffer sizes.
#define DCFG_MAX_THREADS 1024
#define DCFG_CACHELINE_SIZE 64

namespace loop_profiler {

    KNOB<string>knobDcfgFileName(KNOB_MODE_WRITEONCE,
                                 "pintool", "loop-profiler:dcfg-file", "",
                                 "Input this DCFG JSON file containing loop definitions"
                                 " and track loop statistics.");
    KNOB<string>knobStatFileName(KNOB_MODE_WRITEONCE,
                                 "pintool", "loop-profiler:loop-stat-file", "loop-profile.csv",
                                 "Write loop statistics to this file.");
    KNOB<string>knobSep(KNOB_MODE_WRITEONCE,
                        "pintool", "loop-profiler:separator", ",",
                        "Separator between columns in loop-stat-file file.");
    KNOB<BOOL>knobTrace(KNOB_MODE_WRITEONCE,
                        "pintool", "loop-profiler:trace-loops", "0",
                        "Print all loop entries and exits.");
    KNOB<UINT32>knobDebug(KNOB_MODE_WRITEONCE,
                        "pintool", "loop-profiler:debug-level", "0",
                        "Print debug info. Levels: 0 (none), "
                          "1 (summary), 2 (+ loops & instrumentation), 3 (+ analysis).");

    // Maps to keep loop data by ID.
    typedef pair<DCFG_ID, DCFG_LOOP_CPTR> LoopPair;
    typedef unordered_map<DCFG_ID, DCFG_LOOP_CPTR> LoopMap;
    typedef unordered_multimap<DCFG_ID, DCFG_LOOP_CPTR> LoopMultimap;

    // A stack of DCFG IDs.
    // (Using vector<> to access all elements.)
    typedef vector<DCFG_ID> IdStack;

    // Data to track for each loop.
    struct LoopData {

        // Number of interations and entries.
        UINT64 numTrips, numEntries;

        // Sum of nesting depths (used for calculating average depth).
        UINT64 sumDepths;

        // Number of instructions executed in just
        // this loops and all child loops.
        UINT64 numInstrsSelf, numInstrsNested;

        LoopData(): numTrips(0), numEntries(0),
                    sumDepths(0),
                    numInstrsSelf(0), numInstrsNested(0) { }

        static void printHeader(ostream& os) {
            string sep = knobSep.Value();
            os << "num loop entries" <<
                sep << "num iterations" <<
                sep << "ave iterations/entry" <<
                sep << "ave loop-nesting depth" <<
                sep << "num instrs (self)" <<
                sep << "num instrs (nested)" <<
                sep << "ave num instrs/iteration (self)" <<
                sep << "ave num instrs/iteration (nested)";
        }

        float ratio(UINT64 num, UINT64 den) const {
            if (den == 0)
                return 0.f;
            else
                return float(num) / float(den);
        }

        void printData(ostream& os) const {
            string sep = knobSep.Value();
            os << numEntries <<
                sep << numTrips <<
                sep << ratio(numTrips, numEntries) <<
                sep << ratio(sumDepths, numEntries) <<
                sep << numInstrsSelf <<
                sep << numInstrsNested <<
                sep << ratio(numInstrsSelf, numTrips) <<
                sep << ratio(numInstrsNested, numTrips);
        }
    };

    // Loop data per loop ID.
    typedef map<DCFG_ID, LoopData> LoopDataMap;

    // Thread-specific data structure, used during runtime to collect data
    // on a per-thread basis.
    struct ThreadData
    {
        // The previous BB.
        // Used for determining edges.
        DCFG_ID prevBb;

        // A stack to track loop nesting.
        // Current active loop is on top.
        IdStack loopStack;

        // Loop data per loop.
        LoopDataMap loopDataMap;

        ThreadData() : prevBb(0) { }
    };

    // A pointer to ThreadData padded to the size of a cache line.
    // This ensures that pointers can be accessed without
    // causing false-sharing in the cache.
    class ThreadDataPtr
    {
        ThreadData* tdp;
        UINT8 pad[DCFG_CACHELINE_SIZE - sizeof(ThreadData*)];

    public:
        ThreadDataPtr() : tdp(NULL) { }

        ~ThreadDataPtr() {
            freeMem();
        }

        inline ThreadData& operator*() {
            allocMem();
            return *tdp;
        }
        inline const ThreadData& operator*() const {
            ASSERTX(tdp);
            return *tdp;
        }
        inline ThreadData* operator->() {
            allocMem();
            return tdp;
        }
        inline const ThreadData* operator->() const {
            ASSERTX(tdp);
            return tdp;
        }

        // Initialize data struct for a thread.
        inline void allocMem()
        {
            if (!tdp)
                tdp = new ThreadData;
        }

        void freeMem()
        {
            if (tdp)
                delete tdp;
            tdp = NULL;
        }
    };

    class LOOP_PROFILER {

        // Highest thread id seen during runtime.
        UINT32 highestThreadId;

        // Data from DCFG.
        DCFG_DATA* dcfg;

        // Current process in DCFG.
        DCFG_PROCESS_CPTR curProc;

        // First BB executed.
        DCFG_BASIC_BLOCK_CPTR firstBb;

        // Currently active DCFG images.
        set<DCFG_ID> activeImageIds;

        // Loop heads, entry edges, and exit edges.
        // All these maps contain pointers to loop info.
        LoopMap loopHeads;       // keys are loop-head (node) IDs.
        LoopMap loopEntryEdges;    // keys are edge IDs.
        LoopMultimap loopExitEdges; // keys are edge IDs (an edge can exit multiple loops).

        // per-thread data-structure array
        ThreadDataPtr *threadDataArray;

    public:
        LOOP_PROFILER() : highestThreadId(0), dcfg(0), curProc(0), firstBb(0) {

            // This assumes 'new' alignment is on a ptr-sized boundary so
            // pointer will not be split across cache lines and each pointer
            // will be on a separate cache line (pad may split cache lines,
            // but that is ok).
            threadDataArray = new ThreadDataPtr[DCFG_MAX_THREADS];
            ASSERTX(threadDataArray);
        }

        ~LOOP_PROFILER() {
            delete[] threadDataArray;
        }

        // Get the per-thread data for thread tid.
        // Lazy-allocates as needed.
        inline ThreadData& getThreadData(int tid) {
            return *threadDataArray[tid];
        }
        inline const ThreadData& getThreadData(int tid) const {
            return *threadDataArray[tid];
        }

        // Get current inner loop data.
        // If not in any loop, data is for loop "0".
        inline LoopData& getInnerLoopData(int tid) {
            ThreadData& td = getThreadData(tid);
            DCFG_ID loopId = 0; // not in any loop.
            if (!td.loopStack.empty())
                loopId = td.loopStack.back();
            return td.loopDataMap[loopId];
        }

        // Return input string or 'unknown' if NULL, quoted.
        string safeStr(const string* str) const {
            return string("\"") + (str ? *str : "unknown") + "\"";
        }

        // Print results.
        void printData() const {

            ofstream os;
            os.open(knobStatFileName.Value().c_str(),
                         ios_base::out);
            if (!os.is_open()) {
                cerr << "Error: cannot open '" << knobStatFileName.Value() <<
                    "' for saving statistics." << endl;
                return;
            }

            string sep = knobSep.Value();

            // output averages to 2 decimal places.
            os << setprecision(2) << fixed;

            // Header.
            os << "loop id" <<
                sep << "thread id" <<
                sep << "image name" <<
                sep << "symbol name" <<
                sep << "source file" <<
                sep << "source line number" <<
                sep << "loop addr" <<
                sep;
            LoopData::printHeader(os);
            os << endl;

            // Whole program:
            // Threads.
            for (UINT32 tid = 0; tid <= highestThreadId; tid++) {
                const ThreadData& td =  getThreadData(tid);

                // Loop "0" in thread is the program entry.
                LoopDataMap::const_iterator ldi = td.loopDataMap.find(0);
                if (ldi == td.loopDataMap.end())
                    continue;
                const LoopData& loopData = ldi->second;

                ASSERTX(firstBb);
                DCFG_IMAGE_CPTR img = curProc->get_image_info(firstBb->get_image_id());
                ASSERTX(img);

                os << "0" <<
                    sep << tid <<
                    sep << safeStr(img->get_filename()) <<
                    sep << safeStr(firstBb->get_symbol_name()) <<
                    sep << safeStr(firstBb->get_source_filename()) <<
                    sep << firstBb->get_source_line_number() <<
                    sep << (void*)firstBb->get_first_instr_addr() <<
                    sep;
                loopData.printData(os);
                os << endl;
            }

            // Loops.
            for (LoopMap::const_iterator li = loopHeads.begin();
                 li != loopHeads.end(); li++) {
                DCFG_ID loopId = li->first;
                //DCFG_LOOP_CPTR loop = li->second;

                // Threads.
                for (UINT32 tid = 0; tid <= highestThreadId; tid++) {
                    const ThreadData& td =  getThreadData(tid);

                    // Loop in thread.
                    LoopDataMap::const_iterator ldi = td.loopDataMap.find(loopId);
                    if (ldi == td.loopDataMap.end())
                        continue;
                    const LoopData& loopData = ldi->second;
                    DCFG_BASIC_BLOCK_CPTR bb = curProc->get_basic_block_info(loopId);
                    ASSERTX(bb);
                    DCFG_IMAGE_CPTR img = curProc->get_image_info(bb->get_image_id());
                    ASSERTX(img);

                    os << loopId <<
                        sep << tid <<
                        sep << safeStr(img->get_filename()) <<
                        sep << safeStr(bb->get_symbol_name()) <<
                        sep << safeStr(bb->get_source_filename()) <<
                        sep << bb->get_source_line_number() <<
                        sep << (void*)(bb->get_first_instr_addr()) <<
                        sep;
                    loopData.printData(os);
                    os << endl;
                }
            }

            os.close();
        }

        // Find data from DCFG.
        void processDcfg() {
            if (!dcfg)
                return;

            // Processes.
            DCFG_ID_VECTOR processIds;
            dcfg->get_process_ids(processIds);
            if (processIds.size() != 1) {
                cerr << "Error: DCFG file contains " << processIds.size() <<
                    " processes; expected exactly one." << endl;
                exit(1);
            }

            // Get one and only process.
            UINT32 pid = processIds[0];
            curProc = dcfg->get_process_info(pid);
            ASSERTX(curProc);
            if (knobDebug.Value() >= 1)
                cout << "Looking for loop data in process " << pid << "..." << endl;

            // Find all loops.
            DCFG_ID_VECTOR loopIds;
            curProc->get_loop_ids(loopIds);
            for (size_t li = 0; li < loopIds.size(); li++) {
                DCFG_ID loopId = loopIds[li];
                DCFG_LOOP_CPTR loop = curProc->get_loop_info(loopId);
                ASSERTX(loop);
                ASSERTX(loop->get_loop_id() == loopId);

                // Save it (should be only one).
                ASSERTX(loopHeads.count(loopId) == 0);
                loopHeads[loopId] = loop;

                // Get all the entry edges of this loop.
                DCFG_ID_VECTOR entryEdgeIds;
                loop->get_entry_edge_ids(entryEdgeIds);
                if (knobDebug.Value() >= 2)
                    cout << "  Loop " << loopId << " has " << entryEdgeIds.size() <<
                        " entry edge(s):" << endl;
                for (size_t ei = 0; ei < entryEdgeIds.size(); ei++) {
                    DCFG_ID entryEdgeId = entryEdgeIds[ei];

                    // Save this edge (should be only one).
                    ASSERTX(loopEntryEdges.count(entryEdgeId) == 0);
                    loopEntryEdges[entryEdgeId] = loop;
                    if (knobDebug.Value() >= 2)
                        cout << "  - " << entryEdgeId << endl;
                }

                // Get all the exiting edges of this loop.
                DCFG_ID_VECTOR exitEdgeIds;
                loop->get_exit_edge_ids(exitEdgeIds);
                if (knobDebug.Value() >= 2)
                    cout << "  Loop " << loopId << " has " << exitEdgeIds.size() <<
                        " exit edge(s):" << endl;
                for (size_t ei = 0; ei < exitEdgeIds.size(); ei++) {
                    DCFG_ID exitEdgeId = exitEdgeIds[ei];

                    // Save this edge.
                    loopExitEdges.insert(LoopPair(exitEdgeId, loop));
                    if (knobDebug.Value() >= 2)
                        cout << "  - " << exitEdgeId << endl;
                }
            }

            if (knobDebug.Value() >= 1)
                cout << "Tracking " << loopIds.size() << " loop(s)..." << endl;
        }

        // Process DCFG and add instrumentation.
        void activate()
        {
            string dcfgFilename = knobDcfgFileName.Value();
            if (dcfgFilename.length() == 0) {
                if (knobDebug.Value() >= 1)
                    cout << "Not tracking loops because no DCFG input file given." << endl;
                return;
            }

            // Make a new DCFG object.
            dcfg = DCFG_DATA::new_dcfg();

            // Read from file.
            if (knobDebug.Value() >= 1)
                cout << "Reading DCFG from '" << dcfgFilename << "'..." << endl;
            string errMsg;
            if (!dcfg->read(dcfgFilename, errMsg)) {
                cerr << "loop-profiler: " << errMsg <<
                    "; use " << knobDcfgFileName.Cmd() << endl;
                exit(1);
            }

            // Get data from DCFG.
            processDcfg();

            // Add Pin instrumentation.
            TRACE_AddInstrumentFunction(handleTrace, this);
            IMG_AddInstrumentFunction(loadImage, this);
            IMG_AddUnloadFunction(unloadImage, this);
            PIN_AddThreadStartFunction(threadStart, this);
            PIN_AddFiniFunction(printStats, this);
        }
        

        ////// Pin analysis and instrumentation routines.

        // Analysis routine for a DCFG basic block.
        static VOID PIN_FAST_ANALYSIS_CALL
        enterBb(UINT32 bbId,
                LOOP_PROFILER *lt,
                DCFG_BASIC_BLOCK_CPTR bb, // pointer to DCFG BB.
                DCFG_LOOP_CPTR loop, // pointer to DCFG LOOP if this is a loop head.
                THREADID tid) {

            if (knobDebug.Value() >= 3)
                cout << "analyzing BB " << bbId <<
                    ", lt=" << (void*)lt <<
                    ", bb=" << (void*)bb <<
                    ", loop=" << (void*)loop << endl;

            ASSERTX(bbId == bb->get_basic_block_id());
            ThreadData& td = lt->getThreadData(tid);
            IdStack& ls = td.loopStack;

            // Remember 1st BB for output.
            if (tid == 0 && lt->firstBb == 0)
                lt->firstBb = bb;

            // What edge goes from the prev BB to this BB?
            DCFG_ID edgeId = lt->curProc->get_edge_id(td.prevBb, bbId);

            // Are we exiting one or more loops?
            if (edgeId) {

                // Get begin and end iterators bounding the set of loops
                // that are exited with this edge.
                pair<LoopMultimap::iterator, LoopMultimap::iterator> lis =
                    lt->loopExitEdges.equal_range(edgeId);

                // Make set of loop IDs that are exited from this node.
                set<DCFG_ID> exitedLoopIds;
                for (LoopMultimap::iterator li = lis.first;
                     li != lis.second; li++) {
                    DCFG_LOOP_CPTR exitedLoop = li->second;
                    exitedLoopIds.insert(exitedLoop->get_loop_id());
                }

                // Pop off loop stack until done.
                while (!exitedLoopIds.empty()) {

                    // Current loop is at top of stack.
                    DCFG_ID curLoop = ls.empty() ? 0 : ls.back();

                    // Is this loop being exited?
                    if (exitedLoopIds.count(curLoop)) {
                        if (knobTrace.Value()) {
                            for (size_t i = 0; i < ls.size(); i++)
                                cout << "|";
                            cout << " exiting loop " << curLoop << endl;
                        }
                        exitedLoopIds.erase(curLoop);
                        td.loopStack.pop_back();
                    }

                    // Can get here with certain forms of recursion
                    // or unstructured code.
                    else {
                        if (knobDebug.Value() >= 3)
                            cout << "Note: loop exit detected at " << bbId <<
                                ", but not from loop " << curLoop <<endl;
                        break;
                    }
                }
            }

            // Are we entering a loop?
            if (loop && edgeId) {
                LoopMap::iterator li = lt->loopEntryEdges.find(edgeId);

                // Is this edge a loop entry?
                if (li != lt->loopEntryEdges.end()) {
                    DCFG_LOOP_CPTR newLoop = li->second;
                    DCFG_ID newLoopId = newLoop->get_loop_id();
                    ASSERTX(newLoopId == bbId);

                    // Push this loop onto stack.
                    ls.push_back(bbId);
                    if (knobTrace.Value()) {
                        for (size_t i = 0; i < ls.size(); i++)
                            cout << "|";
                        cout << " entering loop " << bbId << endl;
                    }

                    // Update stats.
                    LoopData& ild = lt->getInnerLoopData(tid);
                    ild.numEntries++;
                    ild.sumDepths += ls.size();
                }
            }

            // Final loop context after any exits or entries.
            LoopData& ild = lt->getInnerLoopData(tid);

            // Is this the start of a loop iteration?
            if (loop) {
                ASSERTX(loop->get_loop_id() == bbId);

                // Update stats.
                ild.numTrips++;
            }

            // Num instrs in this loop only.
            UINT64 numInstrs = bb->get_num_instrs();
            ild.numInstrsSelf += numInstrs;

            // Num instrs in all active loops on stack.
            // Use a set to exclude recursion.
            set<DCFG_ID> processedLoopIds;
            for (size_t si = 0; si <= ls.size(); si++) {

                // Special case at end to get "0" loop.
                DCFG_ID loopId = 0;
                if (si < ls.size())
                    loopId = ls[si];

                // Already done?
                if (processedLoopIds.count(loopId))
                    continue;

                // Add counts.
                LoopData& sld = td.loopDataMap[loopId];
                sld.numInstrsNested += numInstrs;
            }

            // Remember this BB for next edge.
            td.prevBb = bbId; 
        }

        // called when an image is loaded.
        static VOID loadImage(IMG img, VOID *v)
        {
            LOOP_PROFILER *lt = static_cast<LOOP_PROFILER *>(v);
            ASSERTX(lt);

            // Get Pin image data.
            UINT32 imgId = IMG_Id(img);
            if (knobDebug.Value() >= 1)
                cout << "Loading image " << imgId <<
                    ", '" << IMG_Name(img) << "' from " <<
                    (void*)IMG_LowAddress(img) << " to " <<
                    (void*)IMG_HighAddress(img) << "..." << endl;

            // Get corresponding DCFG image data.
            DCFG_IMAGE_CPTR dcfgImage = lt->curProc->get_image_info(imgId);
            if (!dcfgImage) {
                cerr << "Warning: image " << imgId << " is not in DCFG; ignoring." << endl;
                return;
            }

            // Check for consistency.
            if (ADDRINT(dcfgImage->get_base_address()) != IMG_LowAddress(img)) {
                cerr << "Warning: image " << imgId <<
                    " is not at DCFG reported address; ignoring." << endl;
                return;
            }

            // Remember.
            lt->activeImageIds.insert(imgId);
        }

        // called when an image is unloaded.
        static VOID unloadImage(IMG img, VOID *v)
        {
            LOOP_PROFILER *lt = static_cast<LOOP_PROFILER *>(v);
            ASSERTX(lt);
            UINT32 imgid = IMG_Id(img);
            lt->activeImageIds.insert(imgid);
            if (knobDebug.Value() >= 1)
                cout << "Unloading image " << imgid <<
                    ", '" << IMG_Name(img) << "'..." << endl;
            lt->activeImageIds.erase(imgid);
        }

        // Handle a new thread.
        static VOID threadStart(THREADID tid, CONTEXT *ctxt,
                                INT32 flags, VOID *v) {
            LOOP_PROFILER *lt = static_cast<LOOP_PROFILER *>(v);
            ASSERTX(lt);
            if (tid > lt->highestThreadId)
                lt->highestThreadId = tid;

            // Set entry data for "0" loop.
            LoopData& ild = lt->getInnerLoopData(tid);
            ild.numEntries = 1;
            ild.numTrips = 1;
        }

        // Add analysis routines when a trace is delivered.
        static VOID handleTrace(TRACE trace, VOID *v)
        {
            LOOP_PROFILER *lt = static_cast<LOOP_PROFILER *>(v);
            ASSERTX(lt);

            // loop thru all BBLs in this trace.
            for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
            {
                // loop thru all the instrs in this BBL.
                for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
                {
                    ADDRINT insAddr = INS_Address(ins);

                    // Get DCFG BBs containing this address.
                    // There will usually be one (or zero if the BB was never executed).
                    // There might be more than one under certain circumstances like
                    // image unload followed by another load.
                    DCFG_ID_VECTOR bbIds;
                    lt->curProc->get_basic_block_ids_by_addr(insAddr, bbIds);
                    if (knobDebug.Value() >= 2)
                        cout << bbIds.size() << " BB(s) at " << (void*)insAddr << endl;
                    for (size_t bbi = 0; bbi < bbIds.size(); bbi++) {
                        DCFG_ID bbId = bbIds[bbi];
                        DCFG_BASIC_BLOCK_CPTR bb = lt->curProc->get_basic_block_info(bbId);
                        ASSERTX(bb);
                        ASSERTX(bb->get_basic_block_id() == bbId);

                        // We only want BBs in active images.
                        DCFG_ID imgId = bb->get_image_id();
                        if (!lt->activeImageIds.count(imgId)) {
                            if (knobDebug.Value() >= 2)
                                cout << "- image " << imgId << " not active" << endl;
                            continue;
                        }
                        
                        // Is the current instr at the beginning of this DCFG BB?
                        // (We only need to instrument the first instr in each BB.)
                        UINT64 bbAddr = bb->get_first_instr_addr();
                        if (bbAddr == insAddr) {
                            if (knobDebug.Value() >= 2)
                                cout << "- is head of BB " << bbId << endl;

                            // Is this BB a loop head?
                            DCFG_LOOP_CPTR loop = NULL;
                            LoopMap::iterator li = lt->loopHeads.find(bbId);
                            if (li != lt->loopHeads.end()) {
                                loop = li->second;
                                ASSERTX(loop->get_loop_id() == bbId);
                                if (knobDebug.Value() >= 2)
                                    cout << "- is head of loop " << bbId << endl;
                            }

                            // Instrument this BB.
                            INS_InsertCall(ins, IPOINT_BEFORE,
                                           (AFUNPTR)enterBb,
                                           IARG_FAST_ANALYSIS_CALL,
                                           IARG_UINT32, bbId,
                                           IARG_PTR, lt,
                                           IARG_PTR, bb,
                                           IARG_PTR, loop,
                                           IARG_THREAD_ID,
                                           IARG_END);
                            if (knobDebug.Value() >= 2)
                                cout << "instrumented BB " << bbId <<
                                    ", lt=" << (void*)lt <<
                                    ", bb=" << (void*)bb <<
                                    ", loop=" << (void*)loop << endl;
                                    
                        }
                    }
                } // INS.
            } // BBL.
        }

        // End of program.
        static VOID printStats(int n, VOID *v)
        {
            LOOP_PROFILER *lt = static_cast<LOOP_PROFILER *>(v);
            ASSERTX(lt);

            if (knobDebug.Value() >= 1)
                cout << "End of program." << endl;
            lt->printData();
        }
    };

} // namespace
#endif
