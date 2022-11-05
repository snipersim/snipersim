// 
// Copyright (C) 2017-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

/*
 The LOOPPOINT class 
*/

#ifndef LOOPPOINT_H
#define LOOPPOINT_H

#include "dcfg_pin_api.H"
#include "pinplay.H"
#include "isimpoint_inst.H"

#include <iomanip>
#include <string>

#if !defined (TARGET_WINDOWS)
#include <sys/cdefs.h>
#endif
#include <unordered_map>

// We use unordered_map because it is more efficient than map for lookups.

using namespace std;
using namespace tr1;
using namespace dcfg_api;
using namespace dcfg_pin_api;

// buffer sizes.
#define DCFG_CACHELINE_SIZE 64

namespace looppoint {

            KNOB<string>knobDcfgFileName(KNOB_MODE_WRITEONCE,
                                         "pintool", "looppoint:dcfg-file", "",
                                         "Input this DCFG JSON file containing loop definitions"
                                         " and track loop statistics.");
            KNOB<UINT32>knobMaxThreads(KNOB_MODE_WRITEONCE,
                                "pintool", "looppoint:max_threads", "256",
                                  "Maximum number of threads supported (default 256).");

            struct LoopInfo {
                ADDRINT entryAddr;
                UINT32 lineNumber;
                const string *fileName;
                UINT64 num_instrs;
                UINT64 num_dynamic_instrs;
                UINT64 num_visits;
                UINT64 num_iterations;
            };

            typedef unordered_map<DCFG_ID, struct LoopInfo * > LoopInfoMap;

            class LOOPPOINT {

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

                LoopInfoMap loopInfoMap;

                ISIMPOINT *isimpointPtr;
                KNOB<string> _MarkerFileKnob;
                KNOB<BOOL> _MainImageOnlyKnob;
                KNOB<BOOL> _SourceLoopsOnlyKnob;
                ofstream mfile;  // for writing out information about loops

            public:
                LOOPPOINT() : highestThreadId(0), dcfg(0), curProc(0),
                    firstBb(0), isimpointPtr(NULL), _MarkerFileKnob(KNOB_MODE_WRITEONCE, "pintool",
                      "looppoint:loop_info", "",
                      "Output file containing loop entries instrumented."),
                    _MainImageOnlyKnob(KNOB_MODE_WRITEONCE, "pintool",
                      "looppoint:main_image_only", "1",
                      "Only instrument main image loops"),
                    _SourceLoopsOnlyKnob(KNOB_MODE_WRITEONCE, "pintool",
                      "looppoint:source_loops_only", "1",
                      "Only instrument loops with source information")
                {

                    // This assumes 'new' alignment is on a ptr-sized boundary so
                    // pointer will not be split across cache lines and each pointer
                    // will be on a separate cache line (pad may split cache lines,
                    // but that is ok).
                }

                // Return input string or 'unknown' if NULL, quoted.
                string safeStr(const string* str) const {
                    return string("\"") + (str ? *str : "unknown") + "\"";
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

                    // process all loops.
                    DCFG_ID_VECTOR loopIds;
                    curProc->get_loop_ids(loopIds);
                    for (size_t li = 0; li < loopIds.size(); li++) {
                        DCFG_ID loopId = loopIds[li];
                        DCFG_LOOP_CPTR loop = curProc->get_loop_info(loopId);
                        ASSERTX(loop);
                        ASSERTX(loop->get_loop_id() == loopId);
                    }

                }

                // Process DCFG and add instrumentation.
                void activate(ISIMPOINT *isimpoint)
                {
                    isimpointPtr = isimpoint;
                    string dcfgFilename = knobDcfgFileName.Value();
                    if (dcfgFilename.length() == 0) {
                        //Not tracking loops because no DCFG input file given.
                        return;
                    }

                    if (strcmp(_MarkerFileKnob.Value().c_str(),"") != 0)
                    {
                        mfile.open(_MarkerFileKnob.Value().c_str());
                        if (!mfile.is_open())
                        {
                            cerr << "Could not open output  file " <<
                                _MarkerFileKnob.Value().c_str() << endl;
                            PIN_ExitApplication(-1);
                        }
                    }

                    // Make a new DCFG object.
                    dcfg = DCFG_DATA::new_dcfg();

                    // Read from file.
                    string errMsg;
                    if (!dcfg->read(dcfgFilename, errMsg)) {
                        cerr << "looppoint: " << errMsg <<
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
                }
        


            // Analysis routine for the entry DCFG basic block for a loop
            static VOID 
            enterLoop( 
                ADDRINT insAddr,
                struct LoopInfo *li,
                LOOPPOINT *lt, 
                UINT32 imageId,
                THREADID tid) {
                if(lt->isimpointPtr->VectorPending(tid))
                {
                    // A slice ended in isimpoint but frequency vector
                    // was not emitted. Do it now.
                    lt->isimpointPtr->EmitVectorForFriend(insAddr, imageId,
                        tid, lt->isimpointPtr, /*markerOffset*/1);
                    // insAddr is the marker captured with IPOINT_BEFORE
                    // for isimpoint we provide an offset of 1
                    // as otherwise the execution in this bbl is not 
                    // going to be counted.
                }
            }

            static VOID ThreadStart(THREADID threadid, CONTEXT *ctxt,
                 INT32 flags, VOID *v)
            {
                if(threadid==knobMaxThreads)
                {
                    cerr << "\tMaximum number of threads (" << 
                        knobMaxThreads << ") reached. \n\t Change with"
                          " -looppoint:max_threads NEWVAL." << endl; 
                    exit(1);
                }
            }

            // called when an image is loaded.
            static VOID loadImage(IMG img, VOID *v)
            {
                LOOPPOINT *lt = static_cast<LOOPPOINT *>(v);
                ASSERTX(lt);

                // Get Pin image data.
                UINT32 imgId = IMG_Id(img);

                // Get corresponding DCFG image data.
                DCFG_IMAGE_CPTR dcfgImage = lt->curProc->get_image_info(imgId);
                if (!dcfgImage) {
                    cerr << "Warning: image " << imgId 
                        << " is not in DCFG; ignoring." << endl;
                    return;
                }

                // Check for consistency.
            if (ADDRINT(dcfgImage->get_base_address()) != IMG_LowAddress(img)) 
            {
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
            LOOPPOINT *lt = static_cast<LOOPPOINT *>(v);
            ASSERTX(lt);
            UINT32 imgid = IMG_Id(img);
            lt->activeImageIds.insert(imgid);
            lt->activeImageIds.erase(imgid);
        }

        static void processLoop(LOOPPOINT *lt, DCFG_ID loopId, 
                struct LoopInfo *loopInfo)
        {
            DCFG_BASIC_BLOCK_CPTR loopIdData = lt->curProc
                                    ->get_basic_block_info(loopId);
            // Get line info
            loopInfo->fileName = loopIdData->get_source_filename();
            loopInfo->lineNumber = loopIdData->get_source_line_number();

            DCFG_LOOP_CPTR loop_cptr = lt->curProc->get_loop_info(loopId);
            ASSERTX(loop_cptr);
            ASSERTX(loop_cptr->get_loop_id() == loopId);
            DCFG_ID_VECTOR loopBbs;
            DCFG_ID_VECTOR entryEdgeIds;
            loop_cptr->get_entry_edge_ids(entryEdgeIds);
            loop_cptr->get_basic_block_ids(loopBbs);
            UINT64 num_instrs = 0;
            UINT64 num_dynamic_instrs = 0;
            UINT64 num_visits = 0;
            for (size_t bi = 0; bi < loopBbs.size(); bi++) {
                DCFG_ID bbId = loopBbs[bi];
                DCFG_BASIC_BLOCK_CPTR bbData = 
                        lt->curProc->get_basic_block_info(bbId);
                num_instrs += bbData->get_num_instrs();
                num_dynamic_instrs += bbData->get_instr_count();
            }
            for (size_t ei = 0; ei < entryEdgeIds.size(); ei++) {
                DCFG_ID entryEdgeId = entryEdgeIds[ei];
                DCFG_EDGE_CPTR entryEdgeData = 
                        lt->curProc->get_edge_info(entryEdgeId);
                num_visits += entryEdgeData->get_exec_count();
            }
            loopInfo->num_instrs = num_instrs;
            loopInfo->num_dynamic_instrs = num_dynamic_instrs;
            loopInfo->num_visits = num_visits;
            loopInfo->num_iterations = loop_cptr->get_iteration_count();
        }

        // Add analysis routines when a trace is delivered.
        static VOID handleTrace(TRACE trace, VOID *v)
        {
            LOOPPOINT *lt = static_cast<LOOPPOINT *>(v);
            ASSERTX(lt);

            RTN r = TRACE_Rtn(trace);
            if (!RTN_Valid(r))  return;

            SEC s = RTN_Sec(r);
            if (!SEC_Valid(s))  return;

            IMG img = SEC_Img(s);
            if (!IMG_Valid(img))  return;

            if(lt->_MainImageOnlyKnob)
            {
                // Only instrument main image loop entries.
                if (!IMG_IsMainExecutable(img)) return; 
            }
            
            // loop thru all BBLs in this trace.
            for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
            {
                INS ins = BBL_InsHead(bbl); 
                // We want to only instrument loop entries that start at
                // Pin BBL heads for easier bbv processing.
                ADDRINT insAddr = INS_Address(ins);

                // Get DCFG BBs containing this address.
                // There will usually be one 
                //  (or zero if the BB was never executed).
                // There might be more than one under certain 
                // circumstances like
                // image unload followed by another load.
                DCFG_ID_VECTOR bbIds;
                lt->curProc->get_basic_block_ids_by_addr(insAddr, bbIds);
                for (size_t bbi = 0; bbi < bbIds.size(); bbi++) 
                {
                    DCFG_ID bbId = bbIds[bbi];
                    DCFG_BASIC_BLOCK_CPTR bb = 
                        lt->curProc->get_basic_block_info(bbId);
                    ASSERTX(bb);
                    ASSERTX(bb->get_basic_block_id() == bbId);
                    UINT64 bbAddr = bb->get_first_instr_addr();

                    // We only want BBs in active images.
                    DCFG_ID imgId = bb->get_image_id();
                    if (!lt->activeImageIds.count(imgId)) {
                        // bb not in an active image. Skip.
                        continue;
                    }
                    DCFG_ID currentLoopId = bb->get_inner_loop_id();
                    // if bbId == currentLoopId, we have a loop-entry bb.
                    if((bbId == currentLoopId) && (insAddr == bbAddr))
                    {
                         LoopInfoMap::const_iterator lit = 
                            lt->loopInfoMap.find(currentLoopId);
                         struct LoopInfo *loopInfo = NULL;
                         if(lit == lt->loopInfoMap.end())
                         {
                            loopInfo = new (struct LoopInfo);

                            loopInfo->entryAddr = insAddr;
                            processLoop(lt, currentLoopId, loopInfo);
                            lt->loopInfoMap[currentLoopId] = loopInfo;
                         }
                         else
                         {
                             loopInfo = lit->second;
                         }
                         // first instruction of the loop entry bb
                         // bb is the loop head
                   
                         if(lt->_SourceLoopsOnlyKnob && 
                                 (loopInfo->lineNumber==0))
                                 continue;
                         if(lt->mfile.is_open())
                         {
                             lt->mfile << "Instrumenting loop id " <<
                                 dec << currentLoopId << 
                                 " insAddr 0x" << hex << insAddr
                                     << endl;
                             if (loopInfo->lineNumber != 0) 
                             {
                                 lt->mfile << " " << *loopInfo->fileName 
                                  << ":" << dec << loopInfo->lineNumber;
                             }
                             else
                             {
                                 lt->mfile << " " << "NoFile:0";
                             }
                             lt->mfile << " #visits " 
                                 << loopInfo->num_visits << " #iterations "
                                 << loopInfo->num_iterations 
                                 << " #static_instructions "
                                 << loopInfo->num_instrs
                                 << " #dynamic_instructions "
                                 << loopInfo->num_dynamic_instrs; 
                             lt->mfile << endl;
                         }
                         INS_InsertCall(ins, IPOINT_BEFORE,
                                 (AFUNPTR)enterLoop,
                                 IARG_ADDRINT, insAddr, 
                                 IARG_PTR, loopInfo,
                                 IARG_PTR, lt,
                                 IARG_UINT32, imgId,
                                 IARG_THREAD_ID,
                                 IARG_END);
                   }
                } // DCFG bbs
            } // BBL.
        }
    };

} // namespace
#endif
