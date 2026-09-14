# QSim frontend

QEMU-based frontend, built around the [QSim](https://github.com/gtcasl/qsim)
library.  **It does not currently build**, and it has no target in the CMake
build.  These notes are what was learned the last time someone tried, kept here
so the next attempt does not start from nothing.

## Why it does not build

`qsim_frontend.cc` was last touched 2021-04-15.  The `frontend/` framework
changed on 2022-10-25 and only `pin_frontend.cc` was brought along with it.
Three things have to change:

1. The framework constructs the frontend with `new T()` (`frontend.tcc:23`), so
   `QsimFrontend` needs a default constructor.  It has only
   `QsimFrontend(FrontendISA)`, which nothing calls — so the ISA selection is
   orphaned and someone has to decide where it now comes from.  This is the one
   part that is a design question rather than a mechanical fix.
2. `__sendInstructionSpecialized` is defined on `Frontend<QsimFrontend>::`,
   which no longer declares it.  `pin_frontend.cc` defines it on the derived
   class (`PinFrontend::__sendInstructionSpecialized`); qsim should match.
3. The `sendInstruction` call at `qsim_frontend.cc:351` is not in scope.

## What a build needs

Checked working before the source stopped compiling; the old makefile was
removed with the rest of the recursive make build.

- The same `CPPFLAGS`, `CXXFLAGS` and link flags as the rest of the tree — in
  particular the C++ standard and `-D_GLIBCXX_USE_CXX11_ABI`, which have to
  match `libsift.a` and `libcarbon_sim.a` or the link fails on mangled names.
  Under CMake that is what linking `sniper_base` gives you.
- Sources: `qsim_frontend.cc` and `../bbv_count.cc`.
- Includes: `..`, `.`, `$QSIM_HOME`, `$QSIM_HOME/include`,
  `$QSIM_HOME/distorm/`.  Both spellings are needed: older QSim installed its
  headers under `include/`, current QSim keeps `qsim.h`, `qsim-load.h` and
  `qsim-regs.h` at the repository root.
- Link: `-L$QSIM_HOME/lib -lqsim $QSIM_HOME/distorm/distorm64.a -lcapstone`,
  plus `libsift.a` and `libcarbon_sim.a`.
- QSim is not fetched by the build.  It has to be cloned and built separately,
  and building it means building its QEMU fork.

## Alternative

Needing a QEMU fork and a QEMU rebuild is the main cost here.  QEMU's own TCG
plugin interface would let a frontend be an ordinary `dlopen`ed `.so` loaded
with `-plugin`, against a stock QEMU.  Whether that replaces this frontend or
sits beside it is still open.
