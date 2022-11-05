// 
// Copyright (C) 2014-2020 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#ifndef _INTERACTIVE_LISTENER_H_
#define _INTERACTIVE_LISTENER_H_


#include "pin.H"
#include <string>

namespace CONTROLLER {

const UINT32 SID_SIZE=20;

class INTERACTIVE_LISTENER{
public:
    INTERACTIVE_LISTENER(const std::string& name):
        _name(name), _main_pid(0), _signaled(FALSE), _processExiting(FALSE) 
    {
#if !defined(TARGET_WINDOWS)
        _server_sock = 0;
        _sid_len = 0;
        memset(_sid, 0, sizeof(_sid));
#else
        _event = NULL;
#endif 
    }

    VOID Active();
    inline UINT32 GetClearSignal() {return _signaled;}

#if !defined(TARGET_WINDOWS)
    
    //check atomically if we had a signal
    inline UINT32 CheckClearSignal(){    
        //using inline asm since we have old compilers that do not support
        //the __sync_val_compare_and_swap function
        int value = 1;
        int new_val = 0;
          __asm__ __volatile__("lock; cmpxchgl %1,%2"
                : "=a"(value)
                : "r"(new_val), "m"(_signaled), "0"(value)
                : "memory");
          return value;
    }
#else
    UINT32 CheckClearSignal();
#endif


private:
#if !defined(TARGET_WINDOWS)        
    static VOID PrepareForFini(VOID* v);
    static VOID Fini(INT32, VOID* v);
    static VOID WaitForUserSiganl(VOID* v);
    static VOID MonitorFD(THREADID tid, CONTEXT *ctxt, SYSCALL_STANDARD std, VOID *v);
    static VOID AfterForkInChild(THREADID tid, const CONTEXT* ctxt, void* v);
    static VOID Detach(VOID* v);

    INT32 OpenSocket();

    char _sid[SID_SIZE];
    unsigned int _sid_len;
    std::string _full_file;
    INT32 _server_sock;
#else
    static VOID PrepareForFini(VOID* v);
    static VOID WaitForUserEvent (VOID* v);

    // Windows event handler
    VOID * _event; 
#endif    

    // Main process id
    std::string _name;
    UINT32 _main_pid;
    
    //using volatile since this member accessed from several threads
    volatile INT32 _signaled;
    volatile BOOL _processExiting;
};


} //namespace
#endif
