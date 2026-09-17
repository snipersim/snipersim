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
