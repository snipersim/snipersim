
#include "lll_info.h"

uint32_t LLLInfo::m_cutoff = 0;
bool LLLInfo::m_initialized = false;
Lock LLLInfo::m_lll_lock;
