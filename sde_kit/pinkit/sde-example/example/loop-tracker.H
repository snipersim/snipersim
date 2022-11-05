// 
// Copyright (C) 2017-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

/*
 The LOOP_TRACKER class defined in this file provides functionality for a
 PinPlay tool that tracks some statistics within loops defined by
 inputting a DCFG data file.

 It can be used as a starting point for other tools that
 need to track DCFG basic blocks, edges, and/or loops.
*/

#ifndef LOOP_TRACKER_H
#define LOOP_TRACKER_H

#include "dcfg_pin_api.H"
#include "pinplay.H"

#include <iomanip>
#include <string>

#if !defined (TARGET_WINDOWS)
#  include <sys/cdefs.h>
#endif
#include <unordered_map>

using namespace std;
using namespace tr1;
using namespace dcfg_api;
using namespace dcfg_pin_api;

// buffer sizes.
#define DCFG_CACHELINE_SIZE 64

namespace loop_tracker {

    KNOB<string>knobDcfgFileName(KNOB_MODE_WRITEONCE,
                                 "pintool", "loop-tracker:dcfg-file", "",
                                 "Input this DCFG JSON file containing loop definitions"
                                 " and track loop statistics.");
    KNOB<string>knobStatFileName(KNOB_MODE_WRITEONCE,
                                 "pintool", "loop-tracker:loop-stat-file", "loop-stats.csv",
                                 "Write loop statistics to this file.");
    KNOB<string>knobSep(KNOB_MODE_WRITEONCE,
                        "pintool", "loop-tracker:separator", ",",
                        "Separator between columns in loop-stat-file file.");
    KNOB<string>knobTraceLoops(KNOB_MODE_WRITEONCE,
                        "pintool", "loop-tracker:trace-loops", "",
                        "Trace statements in the given source-level loops.\n Specify a list of 'filename:linenumber' pairs separted by spaces.");
    KNOB<string>knobTraceLoopIds(KNOB_MODE_WRITEONCE,
                        "pintool", "loop-tracker:trace-loopids", "",
                        "Trace statements in the given loopids.\n Specify a list of DCFG loop-ids separted by spaces.");
    KNOB<UINT32>knobDebug(KNOB_MODE_WRITEONCE,
                        "pintool", "loop-tracker:debug-level", "0",
                        "Print debug info. Levels: 0 (none), "
                          "1 (summary), 2 (+ loops & instrumentation), 3 (+ analysis).");

    KNOB<UINT32>knobMaxThreads(KNOB_MODE_WRITEONCE,
                        "pintool", "loop-tracker:max_threads", "256",
                          "Maximum number of threads supported (default 256).");

    // Maps to keep loop data by ID.
    typedef vector<pair<string,UINT32> > LoopLinenumber;
    typedef unordered_map<DCFG_ID, DCFG_ID_VECTOR> LoopBbsMap;

    struct Counter{
        INT64 _counter;
        UINT8 _pad[DCFG_CACHELINE_SIZE - sizeof(_counter)];
    };

    struct BbInfo {
        ADDRINT exitAddr;
        UINT64 count;
    };

    struct StatementInfo {
        INT32 lineNumber;
        string fileName;
        ADDRINT startAddr;
        ADDRINT endAddr;
        DCFG_ID bbId;
        Counter   *execCount; 
    };
    struct LoopInfo {
        INT32 lineNumber;
        const string *fileName;
        ADDRINT entryAddr;
        BOOL *insideLoop;
        Counter *entryCounter;
        Counter *tempEntryCounter;
        Counter *startCounter;  // entryCounter value for the entry with the largest number of iterations
        Counter *endCounter;  // entryCounter value when the largest number of iterations were done
          // (endCounter[t]._counter - startCounter[t]._counter) == the largest number of iterations
          // on any entry for thread t
    };

    typedef vector < struct StatementInfo * > StatementsVector;
    typedef unordered_map<DCFG_ID, StatementsVector > BbStatementsMap;
    typedef unordered_map<DCFG_ID, struct LoopInfo * > LoopInfoMap;
    typedef unordered_map<DCFG_ID, struct BbInfo * > BbInfoMap;

    class LOOP_TRACKER {

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

        LoopLinenumber loopsOfInterest;
        LoopBbsMap loopBbsOfInterest;
        LoopBbsMap loopExitSinkBbsOfInterest;
        vector<DCFG_ID> loopIdsOfInterest;
        vector<DCFG_ID> parsedLoopIdsOfInterest;
        BbStatementsMap bbStatementsMap;
        LoopInfoMap loopInfoMap;

        PINPLAY_ENGINE *pinplayEngine;

    public:
        LOOP_TRACKER() : highestThreadId(0), dcfg(0), curProc(0), firstBb(0) {

            // This assumes 'new' alignment is on a ptr-sized boundary so
            // pointer will not be split across cache lines and each pointer
            // will be on a separate cache line (pad may split cache lines,
            // but that is ok).
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
            os << "Thread id" <<
            sep << "loop id" <<
            sep << "source file" <<
            ":" << "source line number" << 
            sep << "entry-address" << 
            sep << "total-count" << 
            sep << "start-count" << 
            sep << "end-count" << endl;
            for (UINT tId = 0; tId < knobMaxThreads; tId++)
            {
                for(vector<DCFG_ID>::const_iterator it=loopIdsOfInterest.begin();
                    it != loopIdsOfInterest.end(); it++)
                {
                    DCFG_ID loopId = *it;
                    LoopBbsMap::const_iterator lbi = loopBbsOfInterest.find(loopId);
                    ASSERTX(lbi != loopBbsOfInterest.end());
                    DCFG_ID_VECTOR loopBBs = lbi->second;
                    LoopInfoMap::const_iterator lit = loopInfoMap.find(loopId);
                    struct LoopInfo * linfo = lit->second;
                    BbStatementsMap::const_iterator bsi = bbStatementsMap.find(loopId);
                    if(bsi == bbStatementsMap.end())
                    {
                        if (knobDebug.Value() >= 2)
                            cerr << "No statements found for tId " << dec 
                               << tId << " loopId " << loopId 
                               << *(linfo->fileName) << ":" 
                               << linfo->lineNumber << endl;
                        continue;
                    }
                    StatementsVector statements = bsi->second;

                    if(linfo->entryCounter[tId]._counter)
                    {
                        os << dec << tId << sep;
                        os << dec << loopId << sep;
                        os << *(linfo->fileName) << ":"; 
                        os << linfo->lineNumber << sep; 
                        os << hex << "0x" << linfo->entryAddr << sep; 
                        os << dec << linfo->entryCounter[tId]._counter << sep; 
                        if(linfo->startCounter[tId]._counter)
                            os << dec << linfo->startCounter[tId]._counter << sep; 
                        else
                            os << "*NA*" << sep; 
                        if(linfo->endCounter[tId]._counter)
                            os << dec << linfo->endCounter[tId]._counter; 
                        else
                            os << "*NA*"; 
                        os << endl;

                        os << "Thread id" 
                        << sep << "startAddr" 
                        << sep << " endAddr" 
                        << sep << "bbId"
                        << sep << " source file"
                        << ":line number"
                        << sep << " execCount" << endl;
                        for(vector<DCFG_ID>::const_iterator bit=loopBBs.begin();
                            bit != loopBBs.end(); bit++)
                        {
                            DCFG_ID bbId = *bit;
                            bsi = bbStatementsMap.find(bbId);
                            if(bsi == bbStatementsMap.end()) continue;
                            statements = bsi->second;
                            for(StatementsVector::const_iterator sit = statements.begin();
                                sit != statements.end(); sit++) 
                            {
                                if((*sit)->execCount[tId]._counter) 
                                {
                                    size_t pos = (*sit)->fileName.find_last_of("/");
                                    os << dec << tId << sep;
                                    os << "0x" << hex << (*sit)->startAddr << sep ; 
                                    os << "0x" << hex << (*sit)->endAddr << sep ; 
                                    os <<  dec << (*sit)->bbId << sep ;
                                    os <<  (*sit)->fileName.substr(pos+1) << ":"; 
                                    os << dec << (*sit)->lineNumber << sep ; 
                                    os << dec << (*sit)->execCount[tId]._counter; 
                                    os << endl;
                                }
                            }
                        }
                    }
                }
            }
            os.close();
        }

        // Parse knobTraceLoops to find source loops of interest.
        // Also parse knobTraceLoopIds to find loopIds of interest.
        void parseLoopsOfInterest() 
        {
            stringstream loopToTraceStream(knobTraceLoops);
            string looplocation;
            while(getline(loopToTraceStream, looplocation, ' '))
            {
                size_t colonpos = looplocation.find(":");
                if ( colonpos == string::npos)
                {
                    cerr << "-loop-tracker:trace-loops : expected argument format is filename:linenumber pairs separated by spaces." << endl;
                    exit(1);
                }
                string filename =  looplocation.substr(0, colonpos);
                string linestr =  looplocation.substr(colonpos+1);
                loopsOfInterest.push_back(make_pair(filename, atoi(linestr.c_str())));
            }
            stringstream loopIdsToTraceStream(knobTraceLoopIds);
            string loopidstr;
            while(getline(loopIdsToTraceStream, loopidstr, ' '))
            {
                parsedLoopIdsOfInterest.push_back(atoi(loopidstr.c_str()));
            }
        }

        BOOL isLoopOfInterest(DCFG_ID loopId)
        {
            for(vector<DCFG_ID>::const_iterator it=parsedLoopIdsOfInterest.begin();
                    it != parsedLoopIdsOfInterest.end(); it++)
            {
                    if (*it == loopId) return true;
            }
            DCFG_BASIC_BLOCK_CPTR bb = curProc->get_basic_block_info(loopId);
            ASSERTX(bb);
            for(LoopLinenumber::const_iterator it = loopsOfInterest.begin();
                 it != loopsOfInterest.end(); it++) {
                    if((bb->get_source_line_number() == it->second)
                        && (bb->get_source_filename()->find(it->first) != string::npos)) // allowing for full path vs filename matching; not perfect
                    {
                        DCFG_ID_VECTOR entryEdgeIds;
                        DCFG_LOOP_CPTR linfo = curProc->get_loop_info(loopId);
                        unsigned int num_visits = 0;
                        linfo->get_entry_edge_ids(entryEdgeIds);
                        for (size_t ei = 0; ei < entryEdgeIds.size(); ei++) {
                            DCFG_ID entryEdgeId = entryEdgeIds[ei];
                            DCFG_EDGE_CPTR entryEdgeData = curProc->get_edge_info(entryEdgeId);
                            num_visits += entryEdgeData->get_exec_count();
                        }
                        if(num_visits < bb->get_exec_count())
                        {
                            // Avoid 'loop's with #visits==#iterations 
                            if (knobDebug.Value() >= 2)
                                cout << "found " << it->first << ":" << it->second  << " loopId " << loopId << endl;
                            return true;
                        }
                    }
                 }
            return false;
        }

        // Returns true If instruction at insAddr starts lineNumber or  was never seen
        // before for non-statment starting instructions, mark them as ending
        // the lineNumber for now.
        BOOL InsStartsStatment(DCFG_ID bbId, INT32 lineNumber, string insFileName, ADDRINT insAddr, struct StatementInfo **stInfoPtr)
        {
           for(StatementsVector::iterator it = bbStatementsMap[bbId].begin();
                it != bbStatementsMap[bbId].end(); it++) 
           {
            if ((lineNumber == (*it)->lineNumber)
                 && (insFileName.compare((*it)->fileName) == 0))
                {
                   if (knobDebug.Value() >= 2)
                     cout << "found insAddr " <<  hex << insAddr << " belongs to " << (*it)->fileName << ":" << dec <<  (*it)->lineNumber  << " bbId " << bbId << endl;
                    if ((*it)->startAddr == insAddr) // seen before and starts lineNumber
                    {
                       *stInfoPtr = *it; 
                        return true; 
                    }
                    if((*it)->endAddr < insAddr) (*it)->endAddr = insAddr; 
                                             //non-lineNuber-starting instruction, mark it as line
                                             // ending instruction 
                    return false;
                }
           }
           return true;
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

            parseLoopsOfInterest();

            // process all loops.
            DCFG_ID_VECTOR loopIds;
            curProc->get_loop_ids(loopIds);
            for (size_t li = 0; li < loopIds.size(); li++) {
                DCFG_ID loopId = loopIds[li];
                DCFG_LOOP_CPTR loop = curProc->get_loop_info(loopId);
                ASSERTX(loop);
                ASSERTX(loop->get_loop_id() == loopId);

                if (isLoopOfInterest(loopId))
                {
                    loopIdsOfInterest.push_back(loopId);
                    DCFG_ID_VECTOR loopBbs;
                    DCFG_BASIC_BLOCK_CPTR loopIdData = curProc->get_basic_block_info(loopId);
                    UINT32 count = loop->get_basic_block_ids(loopBbs);
                    if (knobDebug.Value() >= 1)
                        cout << "loopId " << loopId << " #basic blocks " << count << endl;
                    loopBbsOfInterest[loopId] = loopBbs;
                    struct LoopInfo *loopInfo = new (struct LoopInfo);
                    loopInfo->lineNumber = loopIdData->get_source_line_number();
                    loopInfo->fileName = loopIdData->get_source_filename();
                    loopInfo->entryAddr = loopIdData->get_first_instr_addr();
                    loopInfo->insideLoop = new BOOL[knobMaxThreads];
                    loopInfo->entryCounter = new Counter[knobMaxThreads];
                    loopInfo->startCounter = new Counter[knobMaxThreads];
                    loopInfo->endCounter = new Counter[knobMaxThreads];
                    loopInfo->tempEntryCounter = new Counter[knobMaxThreads];
                    for (UINT t = 0; t < knobMaxThreads; t++)
                    {
                        loopInfo->insideLoop[t] = FALSE;
                        loopInfo->entryCounter[t]._counter = 0;
                        loopInfo->startCounter[t]._counter = 0;
                        loopInfo->endCounter[t]._counter = 0;
                    }

                    // Get all the exiting edges of this loop.
                    DCFG_ID_VECTOR exitEdgeIds;
                    loop->get_exit_edge_ids(exitEdgeIds);
                    if (knobDebug.Value() >= 1)
                        cout << "  Loop " << loopId << " has " << exitEdgeIds.size() <<
                        " exit edge(s):"; 
                    for (size_t ei = 0; ei < exitEdgeIds.size(); ei++) {
                        DCFG_ID exitEdgeId = exitEdgeIds[ei];
                        if (knobDebug.Value() >= 1)
                            cout << "  - " << exitEdgeId;
                        DCFG_EDGE_CPTR exitEdgeData = curProc->get_edge_info(exitEdgeId);
                        DCFG_ID exitEdgeSink = exitEdgeData->get_target_node_id();
        ;
                        loopExitSinkBbsOfInterest[loopId].push_back(exitEdgeSink);
                        if (knobDebug.Value() >= 1)
                            cout << "  sink  " << exitEdgeSink;
                    }
                    if (knobDebug.Value() >= 1)
                         cout << endl;

                    loopInfoMap[loopId] = loopInfo;
                }
            }

        }

        // Process DCFG and add instrumentation.
        void activate(PINPLAY_ENGINE *pinplay_engine)
        {
            pinplayEngine = pinplay_engine;
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
                cerr << "loop-tracker: " << errMsg <<
                    "; use " << knobDcfgFileName.Cmd() << endl;
                exit(1);
            }

            // Get data from DCFG.
            processDcfg();

            // Add Pin instrumentation.
            TRACE_AddInstrumentFunction(handleTrace, this);
            IMG_AddInstrumentFunction(loadImage, this);
            PIN_AddThreadStartFunction(ThreadStart, 0);
            IMG_AddUnloadFunction(unloadImage, this);
            PIN_AddFiniFunction(printStats, this);
        }
        

        ////// Pin analysis and instrumentation routines.

        // Analysis routine for instructions starting a source-level statement
        static VOID 
        enterStatement( 
                ADDRINT insAddr,
                struct StatementInfo *si,
                THREADID tid) {

            if (knobDebug.Value() >= 2)
                cout << " tid " << tid << " insAddr " << hex << insAddr << "   Entering statement " << si->fileName << dec <<
                    ":" << si->lineNumber << hex << 
                    " startAddr=" << si->startAddr <<
                    " endAddr=" << si->endAddr << endl << flush;
            si->execCount[tid]._counter++;
        }

        // Analysis routine for the entry DCFG basic block for a loop
        static VOID 
        enterLoop( 
                ADDRINT insAddr,
                struct LoopInfo *li,
                THREADID tid) {

            li->entryCounter[tid]._counter++;
            if (knobDebug.Value() >= 2)
                cout << "insAddr " << hex << insAddr << "   loop entry node" 
                << *(li->fileName) << dec <<
                ":" << li->lineNumber << 
                " entryCount " << dec <<  li->entryCounter[tid]._counter << endl;
            if(!li->insideLoop[tid])
            {
                // entering the loop from outside.
                li->tempEntryCounter[tid]._counter = li->entryCounter[tid]._counter;
                li->insideLoop[tid] = TRUE;
            }
        }

        // Analysis routine for the target DCFG basic block for an exit edge
        // for a loop
        static VOID
        enterLoopExitSink( 
                ADDRINT insAddr,
                struct LoopInfo *li,
                THREADID tid) {
            if(li->insideLoop[tid])
            {
                // exited the loop 
                li->insideLoop[tid] = FALSE;
                if (knobDebug.Value() >= 1)
                    cout << "insAddr " << hex << insAddr << "   Exiting loop " 
                    << *(li->fileName) << dec <<
                    ":" << li->lineNumber << 
                    " last visit iterations " << dec << (li->entryCounter[tid]._counter - li->tempEntryCounter[tid]._counter)  << endl;
                if ((li->endCounter[tid]._counter - li->startCounter[tid]._counter) < (li->entryCounter[tid]._counter - li->tempEntryCounter[tid]._counter))
                {
                    li->startCounter[tid]._counter = li->tempEntryCounter[tid]._counter;
                    li->endCounter[tid]._counter = li->entryCounter[tid]._counter;
                }
            }
        }

        // called when an image is loaded.
        static VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
        {
            if(threadid==knobMaxThreads)
            {
                cerr << "\tMaximum number of threads (" << knobMaxThreads << ") reached. \n\t Change with"
                          " -loop-tracker:max_threads NEWVAL." << endl; 
                exit(1);
            }
        }
        // called when an image is loaded.
        static VOID loadImage(IMG img, VOID *v)
        {
            LOOP_TRACKER *lt = static_cast<LOOP_TRACKER *>(v);
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
            LOOP_TRACKER *lt = static_cast<LOOP_TRACKER *>(v);
            ASSERTX(lt);
            UINT32 imgid = IMG_Id(img);
            lt->activeImageIds.insert(imgid);
            if (knobDebug.Value() >= 1)
                cout << "Unloading image " << imgid <<
                    ", '" << IMG_Name(img) << "'..." << endl;
            lt->activeImageIds.erase(imgid);
        }

        static BOOL isBbOfInterest(LOOP_TRACKER *lt, DCFG_ID bbId, DCFG_ID *loop_Id)
        {
           for(LoopBbsMap::const_iterator it = lt->loopBbsOfInterest.begin();
                 it != lt->loopBbsOfInterest.end(); it++) {
            for(DCFG_ID_VECTOR::const_iterator iv = it->second.begin();
                 iv != it->second.end(); iv++) {
                   if (*iv == bbId)
                    { 
                        *loop_Id = it->first; 
                        return true;
                    } 
                }
            }
            return false;
        }


        static BOOL isBbExitSink(LOOP_TRACKER *lt, DCFG_ID bbId, DCFG_ID *loop_Id)
        {
           for(LoopBbsMap::const_iterator it = lt->loopExitSinkBbsOfInterest.begin();
                 it != lt->loopExitSinkBbsOfInterest.end(); it++) {
            for(DCFG_ID_VECTOR::const_iterator iv = it->second.begin();
                 iv != it->second.end(); iv++) {
                   if (*iv == bbId)
                    { 
                        *loop_Id = it->first; 
                        return true;
                    } 
                }
            }
            return false;
        }


        // Add analysis routines when a trace is delivered.
        static VOID handleTrace(TRACE trace, VOID *v)
        {
            LOOP_TRACKER *lt = static_cast<LOOP_TRACKER *>(v);
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
                    for (size_t bbi = 0; bbi < bbIds.size(); bbi++) {
                        DCFG_ID bbId = bbIds[bbi];
                        DCFG_BASIC_BLOCK_CPTR bb = lt->curProc->get_basic_block_info(bbId);
                        ASSERTX(bb);
                        ASSERTX(bb->get_basic_block_id() == bbId);
                        UINT64 bbAddr = bb->get_first_instr_addr();

                        // We only want BBs in active images.
                        DCFG_ID imgId = bb->get_image_id();
                        if (!lt->activeImageIds.count(imgId)) {
                            if (knobDebug.Value() >= 2)
                                cout << "- image " << imgId << " not active" << endl;
                            continue;
                        }

                        DCFG_ID currentLoopId;

                        if (isBbOfInterest(lt, bbId, &currentLoopId)) 
                        {
                            INT32 lineNumber;
                            string insFileName;
                            PIN_GetSourceLocation(insAddr, NULL, &lineNumber, &insFileName);
                            struct StatementInfo *stInfo = NULL;
                            if (lineNumber && lt->InsStartsStatment(bbId, lineNumber, insFileName, insAddr, &stInfo)) 
                            {
                                if (knobDebug.Value() >= 1)
                                    cout << "ins@" << hex << insAddr << " bbId " << dec << bbId << " starts " << insFileName << ":" << lineNumber << endl;
                                if(!stInfo) 
                                {
                                    stInfo = new (struct StatementInfo);
                                    stInfo->lineNumber = lineNumber;
                                    stInfo->fileName = insFileName;
                                    stInfo->startAddr = insAddr;
                                    stInfo->endAddr = insAddr;
                                    stInfo->bbId = bbId;
                                    stInfo->execCount = new Counter[knobMaxThreads];
                                    for (UINT t = 0; t < knobMaxThreads; t++) stInfo->execCount[t]._counter = 0;
                                    lt->bbStatementsMap[bbId].push_back(stInfo);
                                }

                                // Instrument this INS.
                                INS_InsertCall(ins, IPOINT_BEFORE,
                                           (AFUNPTR)enterStatement,
                                           IARG_ADDRINT, insAddr, 
                                           IARG_PTR, stInfo,
                                           IARG_THREAD_ID,
                                           IARG_END);
                            }

                            if((bbId == currentLoopId) && (insAddr == bbAddr))
                            {
                                // first instruction of the loop entry bb
                                if (knobDebug.Value() >= 1)
                                 cout << "ins@" << hex << insAddr << " bbId " << dec << bbId << " entry for loop " << insFileName << ":" << lineNumber << endl;
                                // bb is the loop head
                                INS_InsertCall(ins, IPOINT_BEFORE,
                                    (AFUNPTR)enterLoop,
                                    IARG_ADDRINT, insAddr, 
                                    IARG_PTR, lt->loopInfoMap[currentLoopId],
                                    IARG_THREAD_ID,
                                    IARG_END);
                             }
                        }

                        if ((insAddr == bbAddr) && isBbExitSink(lt, bbId, &currentLoopId)) 
                        {
                            //  ins it the first instruction of bb and
                            // bb is the  sink(target) of a loop exit edge
                            if (knobDebug.Value() >= 1)
                                cout << "ins@" << hex << insAddr << " bbId " << dec << bbId << " exit-sync for loop " << *(lt->loopInfoMap[currentLoopId]->fileName) << ":" << lt->loopInfoMap[currentLoopId]->lineNumber << endl;
                            INS_InsertCall(ins, IPOINT_BEFORE,
                                (AFUNPTR)enterLoopExitSink,
                                IARG_ADDRINT, insAddr, 
                                IARG_PTR, lt->loopInfoMap[currentLoopId],
                                IARG_THREAD_ID,
                                IARG_END);
                        }
                    }
                } // INS.
            } // BBL.
        }

        // End of program.
        static VOID printStats(int n, VOID *v)
        {
            LOOP_TRACKER *lt = static_cast<LOOP_TRACKER *>(v);
            ASSERTX(lt);

            if (knobDebug.Value() >= 1)
                cout << "End of program." << endl;
            lt->printData();
        }
    };

} // namespace
#endif
