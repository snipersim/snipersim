#include "toolreg.h"
#include "log.h"

REG g_toolregs[TOOLREGS_SIZE];

void initToolregs(void)
{
   for(int i = 0; i < TOOLREGS_SIZE; ++i)
      g_toolregs[i] = PIN_ClaimToolRegister();

   LOG_ASSERT_ERROR(REG_valid(g_toolregs[TOOLREGS_SIZE-1]), "Could not claim sufficient tool registers");
}
