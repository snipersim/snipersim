#ifndef __RECORDER_CONTROL_H
#define __RECORDER_CONTROL_H

#include "sift_format.h"
#include "pin.H"

void setInstrumentationMode(Sift::Mode mode);

void beginROI(THREADID threadid, const CONTEXT * ctxt);
void endROI(THREADID threadid, const CONTEXT * ctxt);

ADDRINT handleMagic(THREADID threadid, const CONTEXT * ctxt, ADDRINT gax, ADDRINT gbx, ADDRINT gcx);

ADDRINT translateAddress(ADDRINT addr, ADDRINT size);

void openFile(THREADID threadid);
void closeFile(THREADID threadid);

void findMyAppId();

void initRecorderControl();

bool rtn_in_extrae(RTN routine);

#define ROI_ON_EEVENT 6000020

#endif // __RECORDER_CONTROL_H
