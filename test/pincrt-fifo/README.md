# PinCRT and FIFOs: a minimal reproducer

Sniper's SDE/Pin recorder talks to the simulator over FIFOs.  With more
application threads than simulated cores it deadlocked (see the commit
"Oversubscription: more threads than cores no longer deadlocks the SDE
frontend"): one recorder thread sat in `fopen()` of a FIFO whose reader had
not arrived, and from then on every other recorder thread hung in its own
`fopen()`.  The fix keeps the recorder out of the C library for those opens.
This directory isolates the runtime behaviour behind that hang so it can be
reported to Intel, and runs the same code under glibc and DynamoRIO for
comparison.

One source, `fifo_check.cpp`, builds three ways: natively against glibc, as
a Pin tool against PinCRT, and as a DynamoRIO client against DynamoRIO's
private libc.  `fifo_app.c` is the application the tools run: its first
thread calls `block_here()` and its main thread, after a marker appears,
calls `probe()`; the tools instrument both and run the checks there, on the
application's threads, which is where the recorder ran them.

## The checks

**Check 1, `open(2)` serialisation.**  One thread calls `fopen(fifo, "w")`
on a FIFO that never gets a reader, so it blocks in `open(2)`.  Once it is
blocked, another thread opens an ordinary regular file.  Controls swap the
blocker's `fopen()` for a raw `open(2)` and the probe's `fopen()` for a raw
`open(2)`, to tell a stdio lock from a lock around the system call.

**Check 2, `fcntl(F_SETFL, O_NONBLOCK)`.**  On the read end of an empty
pipe, then `read()`.

## Results

Pin 4.3 (pin-4.3-99850-ce5652921, the kit bundled with SDE 10.13.1), glibc
2.36, DynamoRIO 11.91 cronbuild, gcc 12.2; `./run.sh` output, each command
under a timeout:

| runtime | check 1: fopen probe | blocker raw open(2) | both raw open(2) | check 2: fcntl + read |
|---|---|---|---|---|
| glibc (native) | returns | returns | returns | F_SETFL ok, read -> EAGAIN |
| PinCRT (Pin tool) | **hangs** | **hangs** | **hangs** | **F_SETFL returns -1 EINVAL, read blocks** |
| DynamoRIO, client threads | returns | returns | returns | F_SETFL ok, read -> -EAGAIN (raw return, errno 0) |
| DynamoRIO, app threads (drwrap) | other thread stalls | other thread stalls | -- | -- |

What the PinCRT rows show, with the breadcrumbs `run.sh` prints under
`VERBOSE=1`: the probe thread reaches the tool, its `fprintf(stderr)`
works, and it hangs exactly at the open of the regular file -- also when
neither thread uses stdio at all.  So this is not a stdio lock: **PinCRT
holds a lock around `open(2)` for the duration of the system call**, and one
open that blocks (a FIFO waiting for its peer) stops every other `open()` and
`fopen()` in the tool.  Independently, **PinCRT's `fcntl(F_SETFL)` fails
with EINVAL**, so a tool cannot even make the FIFO open non-blocking after
the fact; `O_NONBLOCK` has to be given to `open()` and the descriptor
reopened to get blocking behaviour back (which is what `sift/zfstream.cc`
now does).

A process hung this way cannot be stopped with SIGTERM: Pin queues the
signal for the application, which never runs again.  `run.sh` escalates to
SIGKILL (`timeout -k`).

DynamoRIO's private libc has neither problem: with the blocker and the
probe on client threads (`dr_create_client_thread`) the second open returns
at once and `fcntl` works (its `read()` returns the raw `-EAGAIN` instead of
`-1`/`errno`, a private-loader quirk).  In the application-thread shape,
though, the application's other thread stops making progress once one
thread is blocked inside a drwrap pre-callback -- also with a raw `open(2)`,
so this is DynamoRIO's thread handling rather than its libc.  A design that
blocks in `open(2)` from an instrumentation callback therefore stalls
there too, for a different reason.  (Also seen: after `dr_exit_process()`
from a client thread while another client thread is blocked in `open(2)`,
`drrun` returns but the application process lingers; `run.sh` names the
run directory on every command line so it can kill such orphans.)  QEMU TCG plugins are ordinary shared
objects in QEMU's own process using the host glibc, with no private C
runtime; the same code behaves as the glibc row (and Sniper's QEMU frontend
opens its SIFT FIFOs through glibc).

## Running it

    ./run.sh [PIN_ROOT]            # default: ../../sde_kit/pinkit
    DR_ROOT=/path/to/DynamoRIO-Linux-* ./run.sh
    VERBOSE=1 ./run.sh             # every process's output, indented

`make PIN_ROOT=...` builds the tool and the application with Pin's own
makefile rules (`makefile.rules`), `make native` the glibc binary,
`make DR_ROOT=... fifo_check_dr.so` the DynamoRIO client.

## Draft report to Intel (Pin)

**Title:** PinCRT serialises `open(2)` across threads and `fcntl(F_SETFL)`
fails with EINVAL

**Environment:** Pin 4.3 (pin-4.3-99850-ce5652921) as bundled with SDE
10.13.1, Linux x86-64 (Debian 12, glibc 2.36), gcc 12.2.

**Symptom:** In a tool, a thread that calls `open()` or `fopen()` on a FIFO
whose peer has not connected yet blocks in `open(2)`, as expected.  While
it is blocked, every other tool thread's `open()`/`fopen()` -- of any file,
including ordinary regular files -- hangs, and does not return until the
first open completes.  `fprintf` and other stdio calls on the other threads
still work, so the lock is around the system call, not the stream table.
Separately, `fcntl(fd, F_SETFL, O_NONBLOCK)` returns -1 with errno EINVAL
and leaves the flag unset (`F_GETFL` confirms), so a tool cannot make a
descriptor non-blocking after opening it.

**Expected:** POSIX/glibc semantics: an open that blocks holds nothing that
other threads' opens need; `F_SETFL` sets the file status flags.

**Reproducer:** this directory (`fifo_check.cpp`, `fifo_app.c`,
`makefile`, `run.sh`).  `./run.sh` prints one line per runtime and check;
natively both checks return, under Pin both hang (the raw-`open(2)`
controls hang too).  The hung process needs SIGKILL.

**Impact:** any tool that connects threads over FIFOs in a peer-arrival
order it does not control (Sniper's SIFT recorder, one FIFO pair per
thread) deadlocks as soon as one peer is late; the workaround is to open
with `O_NONBLOCK` and reopen blocking once the peer is known to be there,
since `F_SETFL` is unavailable.
