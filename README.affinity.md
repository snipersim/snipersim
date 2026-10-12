# Host CPU affinity for trace workers

Host affinity controls where the native threads executing Sniper run. It is
independent of `scheduler/type`, which controls the scheduling of simulated
application threads onto simulated cores.

## Policy

`traceinput/pin_threads` defaults to `true`, preserving the existing policy:

```sh
# Choose an allowed host CPU pool before starting Sniper.
taskset -c 0-31 ./run-sniper -g traceinput/pin_threads=true [other arguments]

# Keep the same allowed pool, but let Linux place and migrate trace workers.
taskset -c 0-31 ./run-sniper -g traceinput/pin_threads=false [other arguments]
```

Replace the CPU list with CPUs available to your job and `[other arguments]`
with the configuration and workload used by your existing Sniper invocation.
With pinning enabled, worker ID `i` uses entry `i % pool_size` of the sorted
startup CPU list. If there are more workers than CPUs, CPUs are reused. Distinct
logical CPU IDs do not necessarily represent distinct physical cores; select
the pool according to the host topology when that distinction matters.

With pinning disabled, the worker does not call `sched_setaffinity`. Workers
inherit the launcher's allowed CPU pool, and Linux can balance them within it.
The option applies to both the default and userspace MimicOS trace loops.

## Why the startup CPU pool must be retained

On Linux, `sched_getaffinity(0, ...)` queries the calling thread. A new pthread
inherits its creator's CPU affinity. A trace worker can create another worker
after the creator has already been pinned:

```text
launcher allowed CPUs: {2, 4, 6, 8}
  worker 0 -> pinned to CPU 2
    creates worker 1 -> inherits {2}
      queries its current mask -> sees only {2}
```

Querying the allowed mask independently in each worker therefore collapses
workers onto the first selected CPU. This is CPU 0 only when CPU 0 is first in
the launcher's allowed set.

`TraceManager` now captures the allowed CPU list during construction, before
starting trace workers. All workers use this immutable list, including workers
created by an already-pinned worker. Affinity query and assignment errors are
reported instead of remaining silent.

This preserves the startup affinity constraint; it does not reserve CPUs or
prevent other processes from using them. The pool is a startup snapshot rather
than a policy for CPU hotplug or dynamic changes to a job's CPU allocation.

## Checking a run

Use a multithreaded application that creates pthreads dynamically, such as the
bundled FFT with `-p N`. Ensure the allowed host pool contains enough physical
cores for a distinct-core experiment. Run both option values with the same
simulator binary, application, input, configuration and allowed CPU pool.

Each worker's startup message includes its simulated thread ID and native host
TID. While the simulator is running, inspect the corresponding host thread:

```sh
# Replace the placeholders with the simulator PID and the logged host TID.
cat /proc/SIMULATOR_PID/task/HOST_TID/status
cat /proc/SIMULATOR_PID/task/HOST_TID/sched
```

For pinned workers, `Cpus_allowed_list` must be the assigned singleton from the
startup pool, including workers created after worker 0. For unpinned workers,
it must remain the full launcher pool. Checking only `sched_getcpu()` at startup
is insufficient: it identifies the CPU at one instant, not the allowed set.
Repeat with a sparse pool (for example `2,4,6,8`, if available) to avoid assuming
that worker IDs are CPU IDs. A one-CPU pool and more workers than pool entries
should still work, with the documented CPU reuse.

## Validation of these changes

The original implementation of this policy was validated in a Linux build
based on `56505e42fd98bca863fac181e769bd3c98d2bb33` with the tests below.
The policy is unchanged by the coding-style cleanup; the reported throughput
measurements are from the original build, not new measurements of this revision:

- Clean build with GCC 15.2.1, `USE_PIN=1 NO_TORCH=1` and
  `CXX='g++ -Wno-template-body'`.
- A 1/2/4/8/16/32/64-core FFT matrix after the startup-pool fix: three runs per
  point, all 21 passed FFT correctness and live worker-affinity checks.
- Two-core functional checks of both policy values passed.
- A 32-core, 32-thread FFT comparison of the configurable version: five runs per
  mode, alternating order, all ten passed FFT correctness and live affinity
  checks. All 353 invariant effective configuration keys matched.

The two commits were also built and checked independently:

- The first commit passed a build and FFT tests with a sparse four-CPU
  pool, eight workers sharing that pool, and two workers sharing one CPU.
- The second commit passed a build and FFT tests with 32 pinned workers,
  32 unpinned workers, and four unpinned workers in a sparse CPU pool. Live
  affinity masks and positive CPU-time progress were checked for each worker.

These are functional regression checks, not replacements for the throughput
measurements below.

The runtime used Pin 3.31, online SIFT, a glibc 2.32 FFT binary, Gainestown
interval cores at 2.66 GHz, a 100 ns barrier quantum, translation disabled and a
128-way directory. The build also contained a separate initialization fix for
`m_tag_directory_present`; that unrelated fix is not part of these affinity
commits. Results do not claim that the unmodified upstream configuration runs
without those environment and configuration accommodations.

The host had two sockets, 96 physical cores and 192 logical CPUs. Both modes
allowed the launcher to use CPUs 0-191; the pinned workers used physical cores
0-31 on socket 0, while unpinned workers could migrate across the full host.
Before each run, all 192 logical CPUs were below 5% busy over ten seconds and
mean host utilization was below 1%. System services remained active.

FFT used a fixed input of 65,536 complex points (`-m 16 -p 32 -t`). Throughput
is total simulated instructions per host ROI second, summed across cores:

| Mode | Runs | Arithmetic mean MIPS | Median MIPS | Min-max MIPS |
| --- | ---: | ---: | ---: | ---: |
| Pinned | 5 | 8.6103 | 8.4084 | 8.1399-9.6188 |
| Unpinned | 5 | 9.2118 | 9.2935 | 8.8948-9.5213 |

Unpinned throughput was 6.99% higher by arithmetic mean and 10.53% higher by
median; it was higher in four of five paired rounds. Ranges overlap. This is a
comparison of placement policies, including socket/SMT/migration effects, not
an isolated measurement of migration overhead or a general performance
promise. The default remains unchanged.
