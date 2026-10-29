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

Time (x86-64): no time the guest gives reaches the host's kernel, and the
guest reads simulated time, never the host's (`sift/guest_time.h`, as the
SIFT recorder and the QEMU plugin; `dr_fe_time.{cc,h}`).  The clocks --
clock_gettime, gettimeofday and time, through the vDSO (whose entries are
replaced: glibc makes no system call) or as system calls, and rdtsc/rdtscp
-- are the simulator's live, and otherwise the one-IPC model's from the
time zero (the client's `-time_start <seconds>`, default 1337000000,
osemu/time_start's).  Live, futex and the sleeps are the simulator's, and
the other timed waits (poll, select, epoll, sigtimedwait, ...) run as probes
between which the simulator lets the thread sleep in simulated time; to a
file they end at their deadlines in the one-IPC model's time.  alarm,
setitimer, the POSIX timers and timerfd are kept by the client and fired at
their expiry in the guest's time.  A clone's child starts at its parent's
time.  On AArch64 the client says at start that it does not do this (the
guest then reads the host's clocks).  `testing/test_apps.py --only
dr-timed-waits --only dr-timers` checks it, to a file and live.

Sources: `dr_frontend.{cc,h}` specialise the generic front-end layer in
`frontend/` (`frontend.h`, `frontend_control`, `frontend_syscall`,
`frontend_threads`, `frontend_callbacks`); `dr_fe_*.{h,tcc}` are the
DynamoRIO bindings for control, locking, memory copies, options, syscalls
and threads.

Test: `testing/test_apps.py --only dr-frontend` (test/dr-frontend) runs the
self-checking FFT through it; the case is skipped when the client or
DynamoRIO is absent.  An ARM64 trace recorded on a board can be simulated
with `config/cortex-a53.cfg` or `cortex-a72.cfg` (ARM=ON for the decoder).
