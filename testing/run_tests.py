#!/usr/bin/env python3
"""
Sniper MMU test runner.

Runs simulation tests locally (no SLURM), validates results, and reports
pass/fail for each configuration × trace combination.

Usage:
  python3 testing/run_tests.py --suite smoke_test
  python3 testing/run_tests.py --suite fragmentation_sweep --instruction-count 1000000
  python3 testing/run_tests.py --suite full_test --timeout 600
"""

import argparse
import os
import subprocess
import sys
import time
from typing import Dict, List, Tuple, Any

# Add parent directory so we can import create_experiments
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common.printutils import Colors, print_header
from create_experiments import load_yaml, resolve_configs, resolve_traces


def parse_sim_out(sim_out_path: str) -> Dict[str, str]:
    """Parse sim.out into a flat key=value dict."""
    results = {}
    try:
        with open(sim_out_path, "r") as f:
            for line in f:
                line = line.strip()
                if "|" in line:
                    parts = [p.strip() for p in line.split("|")]
                    if len(parts) >= 2 and parts[0]:
                        key = parts[0]
                        value = parts[1] if len(parts) > 1 else ""
                        results[key] = value
    except FileNotFoundError:
        pass
    return results


def validate_result(output_dir: str, cfg_name: str) -> Tuple[bool, str]:
    """
    Validate a simulation result directory.
    Returns (passed, message).
    """
    # Support both flat output (sim.out) and structured output (simulation/sim.out)
    sim_out = os.path.join(output_dir, "sim.out")
    sim_cfg = os.path.join(output_dir, "sim.cfg")
    if not os.path.exists(sim_out):
        sim_out = os.path.join(output_dir, "simulation", "sim.out")
    if not os.path.exists(sim_cfg):
        sim_cfg = os.path.join(output_dir, "simulation", "sim.cfg")

    # Check essential output files exist
    if not os.path.exists(sim_out):
        return False, "sim.out missing"
    if not os.path.exists(sim_cfg):
        return False, "sim.cfg missing"

    stats = parse_sim_out(sim_out)

    # Check instructions were simulated
    icount = int(stats.get("Instructions", "0"))
    if icount == 0:
        return False, "zero instructions simulated"

    # Basic sanity: cycles should be positive
    cycles = int(stats.get("Cycles", "0"))
    if cycles == 0:
        return False, "zero cycles"

    ipc = stats.get("IPC", "?")

    # Check cache stats
    nuca_misses = int(stats.get("num cache misses", "0"))
    dram_accesses = int(stats.get("num dram accesses", "0"))

    details = (
        f"instrs={icount:,}, cycles={cycles:,}, IPC={ipc}, "
        f"nuca_misses={nuca_misses:,}, dram={dram_accesses:,}"
    )
    return True, details


def run_single_test(
    sniper_root: str,
    cfg_flags: str,
    trace_path: str,
    output_dir: str,
    instruction_count: int,
    timeout: int,
    userspace_mimicos: bool = False,
    mimicos_binary: str = "",
    mimicos_config: str = "",
) -> Tuple[bool, float, str]:
    """
    Run a single Sniper simulation.
    Returns (success, elapsed_seconds, message).
    """
    os.makedirs(output_dir, exist_ok=True)

    run_sniper = os.path.join(sniper_root, "run-sniper")
    if userspace_mimicos:
        output_prefix = os.path.join(output_dir, "mimicos_output")
        cmd = (
            f"{run_sniper} --no-cache-warming --genstats "
            f"-s stop-by-icount:{instruction_count} --roi "
            f"{cfg_flags} "
            f"-d {output_dir} "
            f"-- {mimicos_binary} {mimicos_config} {trace_path} {output_prefix}"
        )
    else:
        cmd = (
            f"{run_sniper} --no-cache-warming "
            f"-s stop-by-icount:{instruction_count} "
            f"{cfg_flags} "
            f"-d {output_dir} "
            f"--traces={trace_path}"
        )

    t0 = time.time()
    try:
        result = subprocess.run(
            cmd,
            shell=True,
            timeout=timeout,
            capture_output=True,
            text=True,
            cwd=sniper_root,
        )
        elapsed = time.time() - t0

        if result.returncode != 0:
            # Save stderr for debugging
            err_path = os.path.join(output_dir, "test_stderr.log")
            with open(err_path, "w") as f:
                f.write(result.stderr)
            # Extract last meaningful error line
            err_lines = [l for l in result.stderr.strip().split("\n") if l.strip()]
            last_err = err_lines[-1] if err_lines else "unknown error"
            return False, elapsed, f"exit code {result.returncode}: {last_err}"

        return True, elapsed, "completed"

    except subprocess.TimeoutExpired:
        elapsed = time.time() - t0
        return False, elapsed, f"timeout after {timeout}s"
    except Exception as e:
        elapsed = time.time() - t0
        return False, elapsed, str(e)


def resolve_mimicos_binary(sniper_root, override=None):
    """Locate startup_mimicos.

    CMake builds it into the build directory, so there is one per build
    configuration; the old makefile put it in mimicos/build/ and the suite
    definitions still name that path.  Honour an explicit setting first, then
    the selected build directory, then the legacy location.
    """
    if override:
        return os.path.join(sniper_root, override)

    candidates = []
    env_build = os.environ.get("SNIPER_BUILD_DIR")
    if env_build:
        candidates.append(env_build)
    candidates.append(os.path.join(sniper_root, "build", "default"))

    for build_dir in candidates:
        path = os.path.join(build_dir, "bin", "startup_mimicos")
        if os.path.isfile(path):
            return path

    return os.path.join(sniper_root, "build", "default", "bin", "startup_mimicos")


def main():
    parser = argparse.ArgumentParser(
        description="Run Sniper MMU tests locally",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--suite", type=str, nargs="+", required=True,
        help="Test suite(s) from test_config.yaml",
    )
    parser.add_argument(
        "--instruction-count", type=int, default=None,
        help="Override instruction count (default: from YAML)",
    )
    parser.add_argument(
        "--timeout", type=int, default=300,
        help="Per-test timeout in seconds (default: 300)",
    )
    parser.add_argument(
        "--output-dir", type=str, default=None,
        help="Override output root directory",
    )
    parser.add_argument("--no-color", action="store_true")

    args = parser.parse_args()
    if args.no_color:
        Colors.disable()

    # Resolve paths
    testing_dir = os.path.dirname(os.path.abspath(__file__))
    sniper_root = os.path.dirname(testing_dir)
    yaml_path = os.path.join(testing_dir, "test_config.yaml")

    print_header("Sniper MMU Test Runner")
    print(f"  Sniper root: {sniper_root}")
    print(f"  Config:      {yaml_path}")
    print(f"  Suite(s):    {', '.join(args.suite)}")

    data = load_yaml(yaml_path)
    suites_data = data.get("experiment_suites", {})

    # Collect configs and traces from all suites
    all_cfg_keys: List[str] = []
    instruction_count = 0
    trace_suite_name = None
    userspace_mimicos = False
    mimicos_binary = ""
    mimicos_config = ""

    for suite_name in args.suite:
        if suite_name not in suites_data:
            print(f"{Colors.RED}Error: Suite '{suite_name}' not found.{Colors.RESET}")
            print(f"Available: {', '.join(suites_data.keys())}")
            sys.exit(1)
        suite = suites_data[suite_name]
        all_cfg_keys.extend(suite.get("configs", []))
        instruction_count = max(instruction_count, int(suite.get("instruction_count", 10_000_000)))
        trace_suite_name = suite.get("trace_suite", trace_suite_name)
        if suite.get("userspace_mimicos", False):
            userspace_mimicos = True
            mimicos_binary = resolve_mimicos_binary(sniper_root, suite.get("mimicos_binary"))
            mimicos_config = os.path.join(sniper_root, suite.get("mimicos_config", "mimicos/configs/reservethp_32GB.ini"))

    if args.instruction_count:
        instruction_count = args.instruction_count

    # Deduplicate config keys
    seen = set()
    unique_keys = []
    for k in all_cfg_keys:
        if k not in seen:
            seen.add(k)
            unique_keys.append(k)

    configs, _ = resolve_configs(data, unique_keys, testing_dir)
    traces = resolve_traces(data, testing_dir, trace_suite_name)

    if not traces:
        print(f"{Colors.RED}Error: No traces found for suite '{trace_suite_name}'.{Colors.RESET}")
        sys.exit(1)

    output_root = args.output_dir or os.path.join(testing_dir, "test_results")
    os.makedirs(output_root, exist_ok=True)

    total = len(configs) * len(traces)
    print(f"\n  Configs:     {len(configs)}")
    print(f"  Traces:      {len(traces)}")
    print(f"  Total tests: {total}")
    print(f"  Instrs:      {instruction_count:,}")
    print(f"  Timeout:     {args.timeout}s per test")
    print()

    # Run tests
    results: List[Tuple[str, bool, float, str]] = []
    passed = 0
    failed = 0

    for cfg_name, cfg_flags in configs:
        for trace_name, trace_path, _, _ in traces:
            test_id = f"{cfg_name}_{trace_name}"
            test_dir = os.path.join(output_root, test_id)

            print(f"  {Colors.CYAN}RUN{Colors.RESET}  {test_id} ... ", end="", flush=True)

            success, elapsed, msg = run_single_test(
                sniper_root, cfg_flags, trace_path, test_dir,
                instruction_count, args.timeout,
                userspace_mimicos=userspace_mimicos,
                mimicos_binary=mimicos_binary,
                mimicos_config=mimicos_config,
            )

            if success:
                # Validate outputs
                valid, details = validate_result(test_dir, cfg_name)
                if valid:
                    print(f"{Colors.GREEN}PASS{Colors.RESET} ({elapsed:.1f}s) {details}")
                    passed += 1
                    results.append((test_id, True, elapsed, details))
                else:
                    print(f"{Colors.RED}FAIL{Colors.RESET} ({elapsed:.1f}s) validation: {details}")
                    failed += 1
                    results.append((test_id, False, elapsed, f"validation: {details}"))
            else:
                print(f"{Colors.RED}FAIL{Colors.RESET} ({elapsed:.1f}s) {msg}")
                failed += 1
                results.append((test_id, False, elapsed, msg))

    # Summary
    print_header("Results")
    total_run = passed + failed
    color = Colors.GREEN if failed == 0 else Colors.RED
    print(f"  {color}{passed}/{total_run} passed{Colors.RESET}")
    if failed > 0:
        print(f"\n  Failed tests:")
        for test_id, ok, elapsed, msg in results:
            if not ok:
                print(f"    {Colors.RED}FAIL{Colors.RESET} {test_id}: {msg}")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
