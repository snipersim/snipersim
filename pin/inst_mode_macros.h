#ifndef __INST_MODE_MACROS_H
#define __INST_MODE_MACROS_H

#include "pin.H"

#define INSTR_IF_DETAILED(__inst_mode)         ((__inst_mode) == InstMode::DETAILED)
#define INSTR_IF_NOT_DETAILED(__inst_mode)     ((__inst_mode) != InstMode::DETAILED)
#define INSTR_IF_CACHEONLY(__inst_mode)        ((__inst_mode) == InstMode::CACHE_ONLY)
#define INSTR_IF_NOT_CACHEONLY(__inst_mode)    ((__inst_mode) != InstMode::CACHE_ONLY)
#define INSTR_IF_FASTFORWARD(__inst_mode)      ((__inst_mode) == InstMode::FAST_FORWARD)
#define INSTR_IF_NOT_FASTFORWARD(__inst_mode)  ((__inst_mode) != InstMode::FAST_FORWARD)

#define __INSTRUMENT(predicated, condition, trace, ins, point, func, ...) \
   if (condition)                                                         \
      INS_Insert##predicated##Call(ins, point, func, __VA_ARGS__);        \

#define INSTR_GET_MODE(__trace) ((InstMode::inst_mode_t)TRACE_Version(__trace))

#define INSTRUMENT(...)                   __INSTRUMENT(, __VA_ARGS__)
#define INSTRUMENT_IF(...)                __INSTRUMENT(If, __VA_ARGS__)
#define INSTRUMENT_THEN(...)              __INSTRUMENT(Then, __VA_ARGS__)
#define INSTRUMENT_PREDICATED(...)        __INSTRUMENT(Predicated, __VA_ARGS__)
#define INSTRUMENT_IF_PREDICATED(...)     __INSTRUMENT(IfPredicated, __VA_ARGS__)
#define INSTRUMENT_THEN_PREDICATED(...)   __INSTRUMENT(ThenPredicated, __VA_ARGS__)

#endif // __INST_MODE_MACROS_H
