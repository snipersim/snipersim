# Viewing a run's output

Every process in a run -- Sniper, the recorder, the application or the
userspace MimicOS kernel -- writes into one merged stream, made line by line
by `run-sniper` (`tools/run_sniper.py`, "The run stream").  Each line is
stamped with its wall-clock arrival time and tagged with the process that
wrote it, and the whole stream is kept as `<outputdir>/simulation/sim.stdout`:

    2026-09-17T03:59:49.224121 [kernel] [MimicOS] kernel 0: quantum_expired preempted=0x20000 -> SimContextSwitchTo(0x20000)
    2026-09-17T03:59:49.331347 [sniper] [TRACE:1] -- last application exited; requesting shutdown

Producers write each line with a single call (`include/line_out.h`) and keep
stdout line-buffered, so a line is never split by another thread and the
order in the stream is the order lines were written, to within the pipe's
latency.  The second bracket is the producer's own marker: `[TRACE:1]` is
Sniper's trace thread 1, `[MimicOS]` the kernel, `[SIFT:0]` a reader.

## lnav

[lnav](https://lnav.org) reads the stream with the format in this directory:

    lnav -I tools/lnav <outputdir>/simulation/sim.stdout

or, while a run is going, `lnav -I tools/lnav <outputdir>/simulation/sim.stdout`
tails it live.  Several runs' streams given together are merged by timestamp.
The format parses each line into `proc` (which process), `src` and `tid`
(the producer's marker), and `body`, and marks Sniper errors, assertions,
signals and the deadlock report as errors.  Useful from inside lnav:

    :filter-in kernel            only the kernel's lines (regex on the line)
    :filter-out alloc_path       drop a chatty message
    /quantum_expired             search
    ;SELECT proc, count(*) FROM sniper_run GROUP BY proc
    ;SELECT tid, count(*) FROM sniper_run WHERE src = 'TRACE' GROUP BY tid
    i                            histogram of lines (and errors) over time
    o / O                        jump between lines of the same process (opid)

lnav is a single static binary; the release zip at
https://github.com/tstack/lnav/releases has `lnav-<version>-linux-musl-x86_64.zip`.

## Without lnav

The stream is plain text: `grep '\[kernel\]'`, `sort` (timestamps sort
lexically), `cut -d' ' -f2-` to drop stamps.  `testing/test_virtuoso.py`
refuses a run whose output has an unstamped line, using the same regular
expression as the lnav format, so the format and the stream cannot drift.

## The live feed

`run-sniper -s live[:interval_ns] ...` also writes `<outputdir>/simulation/sim.live`:
one JSON record per line, in simulated time, as the run goes (`scripts/live.py`
documents the records).  Every interval it holds, per core, the instructions
retired, the time in detailed and in fast-forward mode, and the CPI stack as
femtoseconds per group -- the groups are `tools/cpistack.py`'s, so the live
stack and the post-hoc one agree -- and the thread table (state, stall reason,
core, application, name).  Between samples it records every thread event
(start, exit, stall, resume, migrate), instrumentation-mode and ROI changes,
markers, and the software layer's events from the userspace MimicOS kernel:
each event posted to a core's queue (`page_fault`, `new_thread`, `thread_exit`,
`quantum_expired`, `shutdown`) and each `switch_to` of a kernel worker to an
application thread.  A dashboard tails it; `testing/test_virtuoso.py`'s
`live_feed` check pins its contents on the two-application run.

## sniper-live: the run in the terminal

    ./run-sniper --live [--live-interval=<ns>] -n 4 -c ... -- app   the easy way: watch the run
    tools/sniper-live --run -- ./run-sniper -n 4 -c ... -- app    the same, started from the tool
    tools/sniper-live <outputdir>          tail sim.live while it runs, or after
    tools/sniper-live --once <outputdir>   one frame to stdout (no terminal needed)

`run-sniper --live` streams the feed through a private pipe into the
dashboard, which owns the screen (the run's own lines go to `sim.stdout`),
and waits at the end for it to be closed with `q`; closing it earlier leaves
the run going.  Without a terminal `--live` writes `sim.live` instead.
`--run` makes a private named pipe, starts the run-sniper command with
`-s live:<interval>:<pipe>` (`--interval` in simulated ns, default 1000) and
reads the feed from memory as the simulation writes it; the run's own
output goes to its `sim.stdout` as always, and quitting the dashboard leaves
the run going.  To attach from a second terminal instead, give the feed a
pipe yourself: `mkfifo /tmp/live; run-sniper -s live:1000:/tmp/live ...;
tools/sniper-live /tmp/live`.

The dashboard draws the feed, scrolling right to left, one column per
sample of simulated time: the CPI stack aggregated over the busy cores (key
`c` cycles through the cores), colored by family -- compute red, branch
yellow, cache and memory hierarchy green, synchronization blue -- with the
current percentages in the legend; a lane per core showing which application
thread it runs and the kernel's events there (N new_thread, F page_fault,
Q quantum_expired, S switch_to, X thread_exit); a lane per Sniper thread
showing running or stalled and why (futex, pause, syscall); the run's own
output in the bottom quarter -- the stamped stream, every process, scrolling
as it arrives, with `l` cycling the filter through all, sniper, kernel and
each application, errors highlighted; and the running totals.  `space`
pauses, `+`/`-` change how many samples a column holds, `q` quits.  Standard library only; `--once` prints the same frame with ANSI
colors on a terminal and plain text in a pipe, which is what the `live_feed`
check renders.
