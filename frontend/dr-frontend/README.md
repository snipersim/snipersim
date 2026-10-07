# The DynamoRIO front end

A DynamoRIO client (`libdr-frontend.so`) that records a SIFT trace of the
application it instruments, for x86-64 and for ARM64.  It is the light-weight
alternative to the SDE recorder on x86-64 and the only user-space front end
for ARM64, where neither SDE nor QEMU user mode with the plugin applies.

Build: `make DYNAMORIO=ON` (fetches and builds DynamoRIO through
`deps/sniper-deps --dynamorio`, then builds the client into
`<build>/lib/libdr-frontend.so`).  On an ARM64 board the `arm64-frontend`
preset builds only `sift` and the client.

Run: `run-sniper --frontend=dr ... -- ./app` on the simulation host, or
`record-trace --frontend=dr -o trace -- ./app` on the board, then
`run-sniper --traces=trace.app0.th0.sift -c cortex-a72` on the host.
`record-trace` runs `drrun -c <build>/lib/libdr-frontend.so` with the ROI,
fast-forward and detailed-instruction options translated to the client's
flags (`frontend/dr-frontend/dr_fe_options.h`).

Sources: `dr_frontend.{cc,h}` specialise the generic front-end layer in
`frontend/` (`frontend.h`, `frontend_control`, `frontend_syscall`,
`frontend_threads`, `frontend_callbacks`); `dr_fe_*.{h,tcc}` are the
DynamoRIO bindings for control, locking, memory copies, options, syscalls
and threads.

Test: `testing/test_apps.py --only dr-frontend` (test/dr-frontend) runs the
self-checking FFT through it; the case is skipped when the client or
DynamoRIO is absent.  An ARM64 trace recorded on a board can be simulated
with `config/cortex-a53.cfg` or `cortex-a72.cfg` (ARM=ON for the decoder).
