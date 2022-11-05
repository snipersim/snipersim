// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#ifndef LOGGER_H
# define LOGGER_H

#include "pin.H"
#include "pinplay.H"
#include <string>

#if defined (SDE_INIT)
#include "sde-control.H"
#else
#include "control_manager.H"
#endif

class REPLAYER;

namespace Logger
{
class LOGGER_IMPL;
}
// Forward declaration
struct PINPLAY_THREADID;


/*
 * Interface to the logger used by a pintool. The tool needs to
 * instantiate a class of this type. Note that the actual implementation is
 * a singleton, which means there can be only one instance of this class
 * per pintool. If two instances are created an assertion will fire.
 *
 * A pintool should instantiate this class as follows.
 *
 * LOGGER logger;
 *
 * Prior to calling PIN_StartProgram() the methods AddFirst() and AddLast()
 * must be called so that the instrumentation is inserted properly.
 *
 * When combined with the REPLAYR class, LOGGER::AddFirst() should be called
 * before REPLAYER::AddFirst() and LOGGER::AddLast() should be called before
 * REPLAYER::AddLast().
 */
class LOGGER
{
  public:
    LOGGER();

    VOID AddFirst(int argc, char *argv[], BOOL execute_at=false);
    VOID AddLast();
    
    UINT64 GetICount(THREADID pin_tid);
    CONTROLLER::CONTROL_MANAGER *GetController();
    string GetBaseName();
    VOID SetBaseName(const string &name);
    PINPLAY_THREADID PinTidToPinPlayTid(THREADID pin_tid);
    VOID RegisterRegionStart(PINPLAY_HANDLER handler,VOID* args);
    VOID RegisterRegionStop(PINPLAY_HANDLER handler,VOID* args);
    string GetRegionBaseName();
    
    // the only implementation of the logger class
    inline static Logger::LOGGER_IMPL * Impl()
    {
        if(!_impl) {
            _impl = create_logger_impl();
        }
        return _impl;
    }

    void ActivateSelector();
    BOOL LoggerInRegion();
    
  private:
      static Logger::LOGGER_IMPL * create_logger_impl();      
      static Logger::LOGGER_IMPL * _impl; // the actual implementation

};

#endif
