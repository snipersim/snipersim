#include "acc_backend.h"
#include "riscv_isa.h"

#include <string.h>

namespace acc
{

bool execute(Backend &b, unsigned op, uint32_t word, const ame::Env &env)
{
   const char *name = rvisa::name(op);
   auto fail = [&](const std::string &why) { b.errors++; env.error(std::string(name) + ": " + why); return false; };

   if (!strcmp(name, "acc.task"))
   {
      int xs1 = -1;
      rvisa::Operand ops[rvisa::MAX_FIELDS + 1];
      unsigned n = rvisa::operands(op, word, ops);
      for (unsigned i = 0; i < n; i++)
         if (!strcmp(ops[i].name, "xs1")) xs1 = (int)ops[i].value;
      if (xs1 < 0) return fail("no descriptor register");
      const uint64_t da = env.read_x(xs1);
      Descriptor d;
      if (!env.read_mem(da, &d, sizeof(d))) return fail("cannot read the task descriptor");
      env.note_access(da, sizeof(d), false);
      if (!d.t || !d.nin || !d.nout || d.t > 4096 || d.nin > 65536 || d.nout > 65536) return fail("descriptor shape out of range");
      std::vector<float> x((size_t)d.t * d.nin), w((size_t)d.nout * d.nin), bias(d.nout, 0.0f);
      if (!env.read_mem(d.x, x.data(), x.size() * 4)) return fail("cannot read X");
      env.note_access(d.x, x.size() * 4, false);
      if (!env.read_mem(d.w, w.data(), w.size() * 4)) return fail("cannot read W");
      env.note_access(d.w, w.size() * 4, false);
      if (d.bias)
      {
         if (!env.read_mem(d.bias, bias.data(), bias.size() * 4)) return fail("cannot read the bias");
         env.note_access(d.bias, bias.size() * 4, false);
      }
      b.y.assign((size_t)d.t * d.nout, 0.0f);
      for (uint32_t t = 0; t < d.t; t++)
         for (uint32_t o = 0; o < d.nout; o++)
         {
            float s = bias[o];
            const float *xr = &x[(size_t)t * d.nin], *wr = &w[(size_t)o * d.nin];
            for (uint32_t k = 0; k < d.nin; k++) s += xr[k] * wr[k];
            b.y[(size_t)t * d.nout + o] = s;
         }
      b.desc = d;
      b.pending = true;
      b.tasks++;
      return true;
   }
   if (!strcmp(name, "acc.wait"))
   {
      if (!b.pending) return true;                 /* nothing outstanding: completes at once */
      if (!env.write_mem(b.desc.y, b.y.data(), b.y.size() * 4)) return fail("cannot write Y");
      env.note_access(b.desc.y, b.y.size() * 4, true);
      b.pending = false;
      return true;
   }
   return fail("not an accelerator task instruction");
}

}
