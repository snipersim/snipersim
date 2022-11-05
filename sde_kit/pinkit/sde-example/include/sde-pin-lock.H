// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_PIN_LOCK_H_)
# define _SDE_PIN_LOCK_H_

#include "pin.H" /* Exception to the rule about including sde-pin.H. That
                  * would result in a circular include. */

#include "sde-base-types.H"
extern "C" {
#include "sde-portability.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>

class sde_pin_lock_api_t 
{
private:
    PIN_LOCK lock;

public:
    // Constructor
    sde_pin_lock_api_t() {
        PIN_InitLock(&lock);
    }
    // Destructor
    ~sde_pin_lock_api_t() {
        release_lock();
    }
    inline void get_lock(sde_int32_t owner=1) {
        PIN_GetLock(&lock,owner);
    }
    inline void release_lock() {
        PIN_ReleaseLock(&lock);
    }
};

class sde_pin_semaphore_api_t 
{
private:
    PIN_SEMAPHORE sem;

public:
    // Constructor
    sde_pin_semaphore_api_t() {
        PIN_SemaphoreInit(&sem);
    }
    // Destructor
    ~sde_pin_semaphore_api_t() {
        PIN_SemaphoreFini(&sem);
    }
    inline void set_semaphore() {
        PIN_SemaphoreSet(&sem);
    }
    inline void clear_semaphore() {
        PIN_SemaphoreClear(&sem);
    }
    inline void wait_on_semaphore() {
        PIN_SemaphoreWait(&sem);
    }
    inline sde_bool_t wait_on_semaphore_timed(sde_uint32_t miliseconds) {
        return PIN_SemaphoreTimedWait(&sem,miliseconds);
    }

};

#endif
