// 
// Copyright (C) 2004-2017 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#ifndef PINPLAY_H
#define PINPLAY_H

#include "pin.H"
#include "control_manager.H"


#define INVALID_PINPLAY_TID  ((UINT32)-1)

// Allow user tools to generate messages into the PinPlay message file
// Useful when trying to resolve ordering issues between PinPlay and tool 
// instrumentation...
extern VOID PP_Message(const string &);
class PINPLAY_IMPL;

typedef VOID (*PINPLAY_HANDLER)(VOID*);

typedef VOID (*PINPLAY_SYNC_CALLBACK)(BOOL, THREADID, THREADID, UINT64, VOID*);

class PINPLAY_ENGINE
{
  public:
    PINPLAY_ENGINE();

    // initializes and add instrumentation depending on whether logger, 
    // replayer or both are activated
    VOID Activate(int argc, char *argv[], BOOL activate_logger,
        BOOL activate_replayer);
    static PINPLAY_IMPL * Impl();
    VOID ReplayerDoBreakpoint(CONTEXT * ctxt, THREADID pin_tid, 
        BOOL after, string msg);
    UINT64 LoggerGetICount(THREADID pin_tid);
    UINT64 ReplayerGetICount(THREADID pin_tid);
    string LoggerGetBaseName();
    VOID LoggerSetBaseName(const string & name);
    string ReplayerGetBaseName();
    CONTROLLER::CONTROL_MANAGER *LoggerGetController();
    UINT32 LoggerGetPinPlayTid(THREADID pintid);
    UINT32 ReplayerGetPinPlayTid(THREADID pintid);
    ADDRINT ReplayerTranslateAddress(ADDRINT addr); 

    BOOL IsLoggerActive() { return _logger_active;}
    BOOL IsReplayerActive() { return _replayer_active;}

    BOOL IsLoggerRecording();
    
    //register to start/stop logging callbacks
    VOID RegisterRegionStart(PINPLAY_HANDLER handler,VOID* args);
    VOID RegisterRegionStop(PINPLAY_HANDLER handler,VOID* args);

    //register callback to be called on synchronization delay
    VOID RegisterSyncCallback(PINPLAY_SYNC_CALLBACK cb, VOID* args);
    
    //retrieve region's base name
    string GetRegionBaseName(THREADID tid);

    //generate procinfo XML file for the run based on image load
    VOID GenerateProcInfo(const string &filename, const string &pname);
    VOID FinalizeProcInfo();
    
    // Return OS pid and os tid
    UINT32 GetOSPid();

    static enum CALL_ORDER PinPlayFirstBeforeCallOrder();
    static enum CALL_ORDER PinPlayLastAfterCallOrder();

  private:
    static PINPLAY_IMPL * _impl;
    BOOL _logger_active;
    BOOL _replayer_active;
};
#endif
