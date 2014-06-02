#ifndef __RECORDER_CONTROL_H
#define __RECORDER_CONTROL_H

#include "sift_format.h"
#include "pin.H"

void setInstrumentationMode(Sift::Mode mode);

void beginROI(THREADID threadid);
void endROI(THREADID threadid);

ADDRINT handleMagic(THREADID threadid, ADDRINT gax, ADDRINT gbx, ADDRINT gcx);

ADDRINT translateAddress(ADDRINT addr, ADDRINT size);

void openFile(THREADID threadid);
void closeFile(THREADID threadid);

void findMyAppId();

void initRecorderControl();

#endif // __RECORDER_CONTROL_H
