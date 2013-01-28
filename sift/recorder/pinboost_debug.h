#ifndef __PINBOOST_DEBUG_H
#define __PINBOOST_DEBUG_H

#include "pinboost_assert.h"

#include "pin.H"

extern bool pinboost_do_debug;

void pinboost_register(const char* name);
void pinboost_debugme(THREADID threadid);

#endif // __PINBOOST_DEBUG_H
