/* A functional model of the RISC-V Attached Matrix Extension (AME, "Ztt")
 * backend, for the QEMU frontend: the unit's state (M registers,
 * accumulators, datatype registers, ownership) lives here, outside QEMU,
 * and instructions are executed against it from the plugin's callbacks
 * (registers and guest memory through the plugin API).  Nothing here
 * depends on QEMU, so the model can be run on the host against a reference.
 *
 * Implementation parameters of this model (the specification leaves them
 * to the implementation): AME_NELEM = 16 (4x4 squares), AME_UNIT_DATATYPE_SIZE
 * = 32, 32 M registers, 16 accumulators; the Int32 (descriptor 0x40000020)
 * and FP32 (0x20300120: exponent width 8, RNE, infinities and denormals,
 * IEEE class) datatypes, so pack_factor is 1 and every square is 16
 * 32-bit elements.  One backend per hart (no sharing between harts yet).
 *
 * Instructions modelled: ame.acquire/ame.release, asettyp/msettyp/
 * agettyp/mgettyp, mls.{rm,cm,st,tst}, mss.{rm,cm,st,tst}, mmulacc.2d,
 * mmulaccneg.2d, mmulatacc.2d, mmulataccneg.2d, mmulbtacc.2d,
 * mmulbtaccneg.2d, mmov.m.a/mmov.a.m/mmov.m.m, mzero.2d.acc/mzero.2d.m,
 * madd.ew/msub.ew/mmul.ew.  Anything else reports an error.  Conditions the
 * specification traps on (use without ownership, datatype mismatch,
 * unsupported datatype) are reported as errors: a plugin cannot raise a
 * guest exception, see the plan. */
#ifndef AME_BACKEND_H
#define AME_BACKEND_H

#include <stdint.h>
#include <functional>
#include <string>

namespace ame
{
   const unsigned N = 4;                  /* square side: AME_NELEM = 16 */
   const unsigned NELEM = N * N;
   const unsigned NUM_M = 32, NUM_ACC = 16;
   const uint32_t DTYPE_INT32 = 0x40000020;   /* standard, signed, 32 bits */
   const uint32_t DTYPE_FP32 = 0x20300120;    /* IEEE class: exp 8, RNE, Inf, denormals, FP, 32 bits */

   struct Backend
   {
      bool owned = false;
      uint32_t Md[NUM_M] = {0}, Ad[NUM_ACC] = {0};
      int32_t M[NUM_M][NELEM] = {{0}};
      int32_t Acc[NUM_ACC][NELEM] = {{0}};
      uint64_t executed = 0, errors = 0;
   };

   /* What an instruction needs from its host: registers, memory, and a way
    * to report the accesses it made (for the trace's footprint records). */
   struct Env
   {
      std::function<uint64_t(unsigned)> read_x;
      std::function<void(unsigned, uint64_t)> write_x;
      std::function<bool(uint64_t, void *, unsigned)> read_mem;
      std::function<bool(uint64_t, const void *, unsigned)> write_mem;
      std::function<void(uint64_t, unsigned, bool)> note_access;   /* addr, bytes, is_store */
      std::function<void(const std::string &)> error;
   };

   /* Execute the AME instruction `op` (an rvisa opcode id of class ame_*)
    * with encoding `word`.  Returns false when it reported an error. */
   bool execute(Backend &b, unsigned op, uint32_t word, const Env &env);
}

#endif
