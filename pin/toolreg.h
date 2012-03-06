#ifndef __TOOLREG_H
#define __TOOLREG_H

#include "fixed_types.h"

#include "pin.H"

// Maintain a list of tool registers claimed by PIN_ClaimToolRegister()

// Number of available TOOLREG_MEMx (TOOLREG_MEM0 .. TOOLREG_MEM0+TOOLREG_NUM_MEM)
#define TOOLREG_NUM_MEM 3

enum toolreg_t {
   TOOLREG_TEMP, // Can contain anything, but will be reused very quickly
   // Insert more permanent users of tool registers here
   TOOLREG_MEM0,
   TOOLREG_MEM1,
   TOOLREG_MEM2,
   TOOLREG_WRITEADDR,
   TOOLREG_EA0,
   TOOLREG_EA1,
   TOOLREG_EA2,
   TOOLREGS_SIZE
};

extern REG g_toolregs[TOOLREGS_SIZE];

void initToolregs(void);

#endif // __TOOLREG_H
