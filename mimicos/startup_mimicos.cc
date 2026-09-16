#include "mimicos.h"

#include "globals.h"
#include "INIReader.h"
#include <unistd.h>
#include "sim_api.h"
#include "argparser.h"
#include "mm/fast_fault_profile.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <memory>
#include <vector>

int main(int argc, char *argv[]) {

    // Proof-of-life: magic instruction with a recognizable marker string.
    // Under SDE syscall emulation, stdout/stderr are dropped, so we rely on
    // SimSetThreadName which Sniper's magic_server logs to stdout.
    SimSetThreadName("startup_mimicos_main_entered");

    /* Phase 1 fast-fault round-trip probe.
       When MIMICOS_FAST_FAULT_SMOKE=1 is set in the environment, fire a
       single SimFastFault magic with a dummy payload and exit early.
       This proves the magic reaches the Sniper-side handler without
       running the full kernel.  Sniper should log:
           [Virtuoso: SimFastFault] #1 core=0 fp_id=42 mean_cyc=5000 ...
       Off by default so normal runs are unaffected. */
    if (const char* env = std::getenv("MIMICOS_FAST_FAULT_SMOKE"); env && env[0] == '1') {
        MemoryAccessTemplate trace[4] = {
            {0, 0, 6, 0, 0x1234},
            {0, 0, 6, 0, 0x5678},
            {1, 1, 3, 0, 0x9ABC},
            {1, 1, 3, 0, 0xDEF0},
        };
        uint64_t payload[7] = {
            27 /* SIM_CMD_FAST_FAULT marker */,
            42 /* fp_id */,
            0xCAFE000 /* frame_pa */,
            5000 /* mean_cycles */,
            4 /* trace_len */,
            reinterpret_cast<uint64_t>(&trace[0]),
            0 /* cursor */,
        };
        fprintf(stderr, "[startup_mimicos] firing SimFastFault smoke probe\n");
        SimFastFault(7, payload);
        fprintf(stderr, "[startup_mimicos] smoke probe returned, exiting\n");
        return EXIT_SUCCESS;
    }

    // Debug: write to stderr (visible even under SDE)
    fprintf(stderr, "[startup_mimicos] main() entered, argc=%d\n", argc);
    for(int i=0;i<argc;i++) fprintf(stderr, "  argv[%d]=%s\n", i, argv[i]);

    ArgumentParser args;
    if (!args.parse(argc, argv)) {
            fprintf(stderr, "[startup_mimicos] args.parse failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[startup_mimicos] args parsed: config=%s sift=%s output=%s\n", args.configFile.c_str(), args.siftFile.c_str(), args.outputFile.c_str());

    std::cout << "[MimicOS]: Configuration file path: " << args.configFile << std::endl;
    std::cout << "[MimicOS]: SIFT file path: " << args.siftFile << std::endl;
    std::cout << "[MimicOS]: Output file path: " << args.outputFile << std::endl;

    try {
        MimicOS* mimicos = MimicOS::getMimicOS(args.configFile, args.outputFile, args.siftFile);

        mimicos->initHandlers();
        mimicos->boot();
        
    } catch (const std::exception& e) {
        std::cerr << "An exception occurred: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "An unknown error occurred." << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}