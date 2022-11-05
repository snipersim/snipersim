// 
// Copyright (C) 2004-2017 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#ifndef REPLAYER_H
#define REPLAYER_H

#include "pin.H"
#include "pinplay.H"

class LOGGER;

namespace Replayer
{
class REPLAYER_IMPL;
class THREAD_REPLAY;
}
// Forward declaration
struct PINPLAY_THREADID;

/*
 * Interface to the replayer used by a pintool. The tool needs to
 * instantiate a class of this type. Note that the actual implementation is
 * a singleton, which means there can be only one instance of this class
 * per pintool. If two instances are created an assertion will fire.
 *
 * A pintool should instantiate this class as follows.
 *
 * REPLAYER replayer;
 *
 * Prior to calling PIN_StartProgram() the methods AddFirst() and AddLast()
 * must be called so that the instrumentation is inserted properly.
 *
 * When combined with the LOGGER class, LOGGER::AddFirst() should be called
 * before REPLAYER::AddFirst() and LOGGER::AddLast() should be called before
 * REPLAYER::AddLast().
 */
class REPLAYER
{
  public:
    // constructor
    REPLAYER();

    // member functions
    VOID AddFirst();
    VOID AddLast();

    UINT64 GetICount(THREADID pin_tid) const;
    string GetBaseName();
    PINPLAY_THREADID PinTidToPinPlayTid(THREADID pin_tid);
    BOOL IsReady();

    ADDRINT translateAddress(ADDRINT addr) const;

    // register the sync callback in the implementation
    VOID RegisterSyncCallback(PINPLAY_SYNC_CALLBACK cb, VOID* args);

    static VOID DoBreakpoint(CONTEXT * ctxt,
                             THREADID pin_tid,
                             BOOL after,
                             string msg);

    // the only implementation of the replayer class
    static Replayer::REPLAYER_IMPL * Impl();
                                   

  private:
    static Replayer::REPLAYER_IMPL * _impl; // the actual implementation
};

#endif
