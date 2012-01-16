#ifndef __THREAD_START_H__
#define __THREAD_START_H__

#include "fixed_types.h"

#include <elf.h>
#include "pin.H"

int spawnThreadSpawner(CONTEXT *ctxt);
VOID copyStaticData(IMG& img);
VOID copyInitialStackData(IntPtr& reg_esp, core_id_t core_id);
VOID copySpawnedThreadStackData(IntPtr reg_esp);
VOID allocateStackSpace();

// Initialize stack attributes
VOID SimPthreadAttrInitOtherAttr(pthread_attr_t *attr);

#endif /* __THREAD_START_H__ */
