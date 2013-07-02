#ifndef __RECORDER_CONTROL_H
#define __RECORDER_CONTROL_H

#include "pin.H"

void beginROI(THREADID threadid);
void endROI(THREADID threadid);

ADDRINT handleMagic(THREADID threadid, ADDRINT gax, ADDRINT gbx, ADDRINT gcx);

void openFile(THREADID threadid);
void closeFile(THREADID threadid);

void findMyAppId();

void initRecorderControl();

#endif // __RECORDER_CONTROL_H
