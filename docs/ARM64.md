# Sniper on ARM64

Sniper simulates ARM64 traces with the Cortex-A53 and Cortex-A72 core models
(`config/cortex-a53.cfg`, `config/cortex-a72.cfg`).  The trace is recorded on
an ARM64 Linux board by the DynamoRIO front end (`frontend/dr-frontend/`) and
simulated on an x86-64 host whose build carries the ARM decoder (Capstone).

## On the board: build the front end

The board builds libsift and the DynamoRIO client only, no simulator:

```bash
deps/sniper-deps --frontend none --arm --dynamorio fetch   # Capstone and DynamoRIO, built from source
cmake --preset arm64-frontend                               # SNIPER_FRONTEND=none, ARM and DynamoRIO on
cmake --build --preset arm64-frontend                       # build/arm64-frontend/lib/libdr-frontend.so
```

`deps/sniper-deps` puts DynamoRIO's build where the runtime configuration
expects it (`dynamorio/build`); a DynamoRIO built elsewhere is named with
`-DDynamoRIO_DIR=<its build>/cmake` at configure time.

## On the board: record a trace

```bash
./record-trace --frontend=dr --roi -o NAME -- ./program args    # NAME.sift, the ROI only
./record-trace --frontend=dr -o NAME -- ./program args          # the whole run
```

`record-trace` runs `drrun -c build/<preset>/lib/libdr-frontend.so` with the
same region, fast-forward, detailed-instruction, syscall-emulation and VMA
options the SDE recorder takes (`frontend/dr-frontend/dr_fe_options.h`).

## On the host: simulate it

```bash
make ARM=ON -j$(nproc)                     # the ARM decoder needs Capstone: deps/sniper-deps --arm fetch
./run-sniper -v --traces=NAME.sift -c cortex-a72
```

## Testing

`testing/test_apps.py --only dr-frontend` records and simulates the FFT
through the DynamoRIO client on x86-64 (the same client code the board
runs).  An ARM64 trace recorded on a board can be checked on the host with
`run-sniper --traces=<trace> -c cortex-a53` and `-c cortex-a72`; the
configuration sweep (`testing/test_configs.py`) skips those two
configurations when no ARM trace is available.
