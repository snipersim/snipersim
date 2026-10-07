
import os
import argparse
import sys
from typing import List, Tuple, Dict, Any
from common.printutils import Colors, print_header


 


def load_yaml(yaml_path: str) -> Dict[str, Any]:
    try:
        import yaml  # type: ignore
    except Exception:
        print("Error: PyYAML not installed. Install via 'pip install pyyaml'.", file=sys.stderr)
        sys.exit(1)

    # Loader that rejects duplicate keys and reports locations
    class UniqueKeyLoader(yaml.SafeLoader):
        pass

    def construct_mapping(loader, node, deep=False):
        seen = set()
        mapping = {}
        for key_node, value_node in node.value:
            key = loader.construct_object(key_node, deep=deep)
            if key in seen:
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    f"found duplicate key: {key}",
                    key_node.start_mark,
                )
            seen.add(key)
            mapping[key] = loader.construct_object(value_node, deep=deep)
        return mapping

    UniqueKeyLoader.construct_mapping = construct_mapping  # type: ignore

    try:
        with open(yaml_path, "r") as f:
            data = yaml.load(f, Loader=UniqueKeyLoader)
        if not isinstance(data, dict):
            raise ValueError("YAML root must be a mapping/dict")
        return data
    except FileNotFoundError:
        print(f"Error: YAML file not found: {yaml_path}", file=sys.stderr)
        sys.exit(1)
    except yaml.constructor.ConstructorError as e:
        # Provide clear duplicate key error with location
        print(f"YAML error: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error loading YAML: {e}", file=sys.stderr)
        sys.exit(1)


def resolve_configs(
    yaml_data: Dict[str, Any],
    suite_config_keys: List[str],
    yaml_dir: str,
) -> Tuple[List[Tuple[str, str]], Dict[str, List[str]]]:
    """
    Resolve configs from YAML into (variant_name, flags) tuples.
    - Validates that each config has at least 'name' and 'file'.
    - Applies 'extends' options (supports list of strings or [id, option] pairs).
    - Expands 'sweeps' into cartesian product across dimensions, appending options
      and suffixes to the name in order of appearance: -<id><value>.
    """
    from itertools import product

    configs_root = yaml_data.get("configs", {})
    base_path_rel = configs_root.get("configs_base_path")
    if not base_path_rel:
        print("Error: 'configs_base_path' missing in YAML.", file=sys.stderr)
        sys.exit(1)

    configs_base_dir = os.path.normpath(os.path.join(yaml_dir, base_path_rel))

    resolved: List[Tuple[str, str]] = []
    origins: Dict[str, List[str]] = {}

    # Pre-sweep duplicate base name check
    base_names: Dict[str, List[str]] = {}
    for cfg_key in suite_config_keys:
        cfg_entry = configs_root.get(cfg_key)
        if not cfg_entry:
            print(f"Error: Config key '{cfg_key}' not defined in YAML.", file=sys.stderr)
            sys.exit(1)
        base_name = cfg_entry.get("name")
        if base_name:
            base_names.setdefault(base_name, []).append(f"configs.{cfg_key}.name")

    dup_base = {n: locs for n, locs in base_names.items() if len(locs) > 1}
    if dup_base:
        print("Error: Duplicate config 'name' entries detected before sweeps:", file=sys.stderr)
        for n, locs in dup_base.items():
            print(f"  name='{n}' at: {', '.join(locs)}", file=sys.stderr)
        sys.exit(1)

    for cfg_key in suite_config_keys:
        cfg_entry = configs_root.get(cfg_key)
        if not cfg_entry:
            print(f"Error: Config key '{cfg_key}' not defined in YAML.", file=sys.stderr)
            sys.exit(1)

        base_name = cfg_entry.get("name")
        cfg_file = cfg_entry.get("file")
        if not base_name or not cfg_file:
            print(
                f"Error: Config '{cfg_key}' must include both 'name' and 'file'.",
                file=sys.stderr,
            )
            sys.exit(1)

        cfg_path = os.path.join(configs_base_dir, cfg_file)
        base_flags = f" -c {cfg_path}"

        # extends: either list of strings, or list of [id, option] pairs (legacy)
        extends = cfg_entry.get("extends", [])
        for ext in extends:
            if isinstance(ext, str):
                base_flags += f" -g {ext}"
            elif isinstance(ext, list) and len(ext) == 2:
                _, opt = ext
                base_flags += f" -g {opt}"
            else:
                print(
                    f"Warning: malformed 'extends' entry in config '{cfg_key}': {ext}",
                    file=sys.stderr,
                )

        sweeps = cfg_entry.get("sweeps", [])
        if sweeps:
            # Build sweep dimensions preserving order.
            # Each dimension is a list of "assignment groups".
            # An assignment group is a list of (identifier, option_prefix, value) tuples applied together.
            dims: List[List[List[Tuple[str, str, Any]]]] = []
            for sw in sweeps:
                sid = sw.get("identifier")
                opt = sw.get("option")
                vals = sw.get("values")
                if not sid or not opt or not isinstance(vals, list) or not vals:
                    print(
                        f"Error: Malformed sweep in config '{cfg_key}'; requires identifier, option, values[].",
                        file=sys.stderr,
                    )
                    sys.exit(1)

                # Case A: single-variable sweep entry
                if isinstance(sid, str) and isinstance(opt, str):
                    # vals must be a flat list
                    dim_groups: List[List[Tuple[str, str, Any]]] = []
                    for v in vals:
                        dim_groups.append([(sid, opt, v)])
                    dims.append(dim_groups)

                # Case B: multi-variable sweep entry with one-to-one mapping across variables
                elif isinstance(sid, list) and isinstance(opt, list):
                    # Validate lengths and structure
                    if not all(isinstance(s, str) for s in sid):
                        print(
                            f"Error: Sweep 'identifier' must be a list of strings in config '{cfg_key}'.",
                            file=sys.stderr,
                        )
                        sys.exit(1)
                    if not all(isinstance(o, str) for o in opt):
                        print(
                            f"Error: Sweep 'option' must be a list of strings in config '{cfg_key}'.",
                            file=sys.stderr,
                        )
                        sys.exit(1)
                    if not all(isinstance(vl, list) for vl in vals):
                        print(
                            f"Error: Sweep 'values' must be a list of lists for multi-variable sweep in config '{cfg_key}'.",
                            file=sys.stderr,
                        )
                        sys.exit(1)
                    if len(sid) != len(opt) or len(opt) != len(vals):
                        print(
                            f"Error: Mismatched lengths in multi-variable sweep in config '{cfg_key}'. "
                            f"Lengths: identifiers={len(sid)}, options={len(opt)}, values={len(vals)}",
                            file=sys.stderr,
                        )
                        sys.exit(1)
                    # Ensure each variable's values list has equal length
                    lengths = [len(vl) for vl in vals]
                    if len(set(lengths)) != 1:
                        print(
                            f"Error: All variables within a multi-variable sweep must have the same number of values in config '{cfg_key}'. "
                            f"Per-variable lengths: {lengths}",
                            file=sys.stderr,
                        )
                        sys.exit(1)

                    # Build assignment groups by index (one-to-one mapping)
                    num_entries = lengths[0]
                    dim_groups: List[List[Tuple[str, str, Any]]] = []
                    for i in range(num_entries):
                        group: List[Tuple[str, str, Any]] = []
                        for j in range(len(sid)):
                            group.append((sid[j], opt[j], vals[j][i]))
                        dim_groups.append(group)
                    dims.append(dim_groups)

                else:
                    print(
                        f"Error: Sweep 'identifier'/'option' types not supported in config '{cfg_key}'.",
                        file=sys.stderr,
                    )
                    sys.exit(1)

            # Cartesian product across dimensions (each dimension contributes an assignment group)
            for combo in product(*dims):
                # Flatten all assignment groups from each dimension
                assignments: List[Tuple[str, str, Any]] = [t for group in combo for t in group]
                name_suffix_parts = []
                flags = base_flags
                for sid_i, opt_i, val_i in assignments:
                    # Python bool str() gives 'True'/'False'; configs expect 'true'/'false'
                    str_val = str(val_i).lower() if isinstance(val_i, bool) else str(val_i)
                    name_suffix_parts.append(f"-{sid_i}{str_val}")
                    flags += f" -g {opt_i}{str_val}"

                variant_name = base_name + "".join(name_suffix_parts)
                resolved.append((variant_name, flags))
                origin = (
                    f"{cfg_key} ({base_name}) "
                    f"[{', '.join([f'{sid}={val}' for sid, _, val in assignments])}]"
                )
                origins.setdefault(variant_name, []).append(origin)
        else:
            # No sweeps: single variant
            resolved.append((base_name, base_flags))
            origin = f"{cfg_key} ({base_name})"
            origins.setdefault(base_name, []).append(origin)
            
    # Post-sweep duplicate variant name check
    dup_variants = {n: locs for n, locs in origins.items() if len(locs) > 1}
    if dup_variants:
        print("Error: Duplicate config variant names after applying sweeps:", file=sys.stderr)
        for n, locs in dup_variants.items():
            print(f"  variant='{n}' produced by:")
            for src in locs:
                print(f"    - {src}", file=sys.stderr)
        sys.exit(1)

    return resolved, origins


def _parse_tlist_with_metrics(tlist_path: str) -> List[Tuple[str, str, str, str]]:
    rows: List[Tuple[str, str, str, str]] = []
    try:
        with open(tlist_path, "r") as f:
            lines = [ln.strip() for ln in f if ln.strip()]
        if not lines:
            return rows
        # The first line is the trace directory; $VARS in it are expanded, so a
        # list can name $SNIPER_TRACES/<set> instead of one host's mount point.
        trace_dir = os.path.expandvars(lines[0])
        for line in lines[1:]:
            parts = [p.strip() for p in line.split(",")]
            if len(parts) == 4:
                tracename, filename, l2tlb_mpki, norm_ptw_lat = parts
                rows.append((tracename, os.path.join(trace_dir, filename), l2tlb_mpki, norm_ptw_lat))
            else:
                print(f"Warning: Skipping malformed tlist entry: {line}", file=sys.stderr)
    except FileNotFoundError:
        print(f"Error: tlist not found: {tlist_path}", file=sys.stderr)
    except Exception as e:
        print(f"Error reading tlist '{tlist_path}': {e}", file=sys.stderr)
    return rows


def resolve_traces(
    yaml_data: Dict[str, Any],
    yaml_dir: str,
    trace_suite_name: str,
) -> List[Tuple[str, str, str, str]]:
    trace_suites = yaml_data.get("trace_suite", {})
    ts = trace_suites.get(trace_suite_name)
    if not ts:
        print(f"Error: Trace suite '{trace_suite_name}' not defined in YAML.", file=sys.stderr)
        sys.exit(1)

    base_rel = ts.get("tracelist_base_path")
    tracelists = ts.get("tracelists", [])
    if not base_rel or not tracelists:
        print(f"Error: Trace suite '{trace_suite_name}' missing base path or tracelists.", file=sys.stderr)
        sys.exit(1)

    trace_list_dir = os.path.join(yaml_dir, base_rel)
    traces: List[Tuple[str, str, str, str]] = []
    for tl in tracelists:
        tl_path = os.path.join(trace_list_dir, tl)
        traces.extend(_parse_tlist_with_metrics(tl_path))
    return traces


def sanitize_dirname(name: str) -> str:
    """Sanitize a string for use in directory/file names.
    
    Replaces problematic characters:
      - ',' -> '-'  (comma in RestSeg sizes like 1024,8192)
      - ' ' -> '-'  (spaces)
      - other special chars that cause issues
    """
    # Replace comma with hyphen (common in RestSeg size specs)
    name = name.replace(",", "-")
    # Replace spaces
    name = name.replace(" ", "-")
    # Remove or replace other problematic characters
    for char in [':', ';', '|', '\\', '/', '*', '?', '"', '<', '>', '!']:
        name = name.replace(char, '-')
    # Collapse multiple hyphens
    while '--' in name:
        name = name.replace('--', '-')
    return name


def build_jobfile(
    jobfile_path: str,
    sniper_path: str,
    instruction_count: int,
    configs: List[Tuple[str, str]],
    traces: List[Tuple[str, str, str, str]],
    output_root: str,
    enable_icache_prefixes: List[str] = None,
) -> List[Tuple[str, str, str, str]]:
    jobs: List[Tuple[str, str, str, str]] = []  # (id, cfg_name, trace_name, out_dir)

    with open(jobfile_path, "w") as jobfile:
        jobfile.write("#!/bin/bash\n")

        counter = 0
        for cfg_name, cfg_flags in configs:
            for trace_name, trace_path, _, _ in traces:
                execution_command = os.path.join(sniper_path, "run-sniper")
                sniper_parameters = f" --no-cache-warming -s stop-by-icount:{instruction_count}"

                # Sanitize names for directory/file usage (remove commas, etc.)
                safe_cfg_name = sanitize_dirname(cfg_name)
                safe_trace_name = sanitize_dirname(trace_name)
                out_dir_name = f"{safe_cfg_name}_{safe_trace_name}"
                output_directory = os.path.join(output_root, out_dir_name)
                os.makedirs(output_directory, exist_ok=True)

                output_command = f" -d {output_directory} "
                trace_arg = f" --traces={trace_path} "

                # Conditionally enable icache modeling for matching trace prefixes
                icache_flags = ""
                if enable_icache_prefixes:
                    for prefix in enable_icache_prefixes:
                        if trace_name.startswith(prefix):
                            icache_flags = " -g --general/enable_icache_modeling=true"
                            break

                command = execution_command + sniper_parameters + cfg_flags + icache_flags + output_command + trace_arg

                sbatch_cmd = (
                    "sbatch --exclude=kratos17 -J {}_{} --output="
                    + os.path.join(output_directory, "slurm.out")
                    + " --error="
                    + os.path.join(output_directory, "slurm.err")
                    + " "
                    + os.path.join(sniper_path, "native_wrapper.sh")
                ).format(safe_cfg_name, safe_trace_name)

                jobfile.write(sbatch_cmd + " \"" + " " + command + "\"")
                jobfile.write("\n")

                # Use sanitized names in job list for CSV compatibility
                jobs.append((str(counter), safe_cfg_name, safe_trace_name, output_directory))
                counter += 1

    return jobs


def write_csv(path: str, header: List[str], rows: List[Tuple[str, ...]]) -> None:
    """Write CSV with proper quoting for fields containing commas."""
    import csv
    with open(path, "w", newline='') as f:
        writer = csv.writer(f, quoting=csv.QUOTE_MINIMAL)
        writer.writerow(header)
        for row in rows:
            writer.writerow(row)


def main():
    parser = argparse.ArgumentParser(
        description="Create experiment suite with jobs and results layout",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --artifact-path . --yaml experiments/clist.yaml --suite perfect_translation --suite-dir-name perfect_translation_251225
  %(prog)s --artifact-path . --yaml experiments/clist.yaml --suite perfect_translation --suite-dir-name perfect_translation_251225 --force
  %(prog)s --artifact-path . --yaml experiments/clist.yaml --suite suite1 suite2 suite3 --suite-dir-name combined_run
        """,
    )

    parser.add_argument("--artifact-path", type=str, required=True, help="Path to the virtuoso artifact root")
    parser.add_argument("--yaml", type=str, default="clist.yaml", help="Path to the experiment YAML")
    parser.add_argument("--suite", type=str, nargs='+', required=True, help="Experiment suite name(s) from YAML (can specify multiple)")
    parser.add_argument(
        "--suite-dir-name",
        type=str,
        default=None,
        help="Override directory name for the suite (defaults to suite name)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Allow reusing an existing suite directory; overwrite lists/jobfile",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        help="Disable colored output",
    )
    parser.add_argument(
        "--enable-icache-for",
        type=str,
        nargs='+',
        default=None,
        metavar="PREFIX",
        help="Enable icache modeling only for traces whose names start with the given prefix(es). "
             "E.g., --enable-icache-for srv  enables icache only for srv* workloads.",
    )

    args = parser.parse_args()

    if args.no_color:
        Colors.disable()

    artifact_path = os.path.abspath(args.artifact_path)
    yaml_path = os.path.abspath(args.yaml)
    yaml_dir = os.path.dirname(yaml_path)

    print_header("Create Experiments Suite")
    data = load_yaml(yaml_path)

    suites_data = data.get("experiment_suites", {})
    
    # Validate all requested suites exist
    suite_names = args.suite  # Now a list
    for suite_name in suite_names:
        if suite_name not in suites_data:
            print(f"Error: Suite '{suite_name}' not found in YAML.", file=sys.stderr)
            available = list(suites_data.keys())
            print(f"Available suites: {', '.join(available)}", file=sys.stderr)
            sys.exit(1)

    # Merge configs and traces from all suites
    all_cfg_names: List[str] = []
    all_trace_suite_names: set = set()
    instruction_count: int = 0
    
    for suite_name in suite_names:
        suite = suites_data[suite_name]
        cfg_names = suite.get("configs", [])
        all_cfg_names.extend(cfg_names)
        all_trace_suite_names.add(str(suite.get("trace_suite")))
        # Use the maximum instruction count across all suites
        suite_instr = int(suite.get("instruction_count", 300_000_000))
        instruction_count = max(instruction_count, suite_instr)
    
    # Remove duplicate config names while preserving order
    seen_cfgs = set()
    unique_cfg_names = []
    for cfg in all_cfg_names:
        if cfg not in seen_cfgs:
            seen_cfgs.add(cfg)
            unique_cfg_names.append(cfg)
    all_cfg_names = unique_cfg_names
    
    # Warn if multiple trace suites are used
    if len(all_trace_suite_names) > 1:
        print(f"Warning: Multiple trace suites specified: {all_trace_suite_names}")
        print("Using first trace suite for now. Consider using a single trace suite.")
    trace_suite_name = list(all_trace_suite_names)[0]

    # Use provided suite-dir-name or generate from suite names
    dir_name = args.suite_dir_name or "_".join(suite_names)
    # Ensure suite directory names are prefixed with 'exp_'
    if not dir_name.startswith("exp_"):
        dir_name = f"exp_{dir_name}"

    # Create suite directory under experiments/<suite_name>, with nested results/ for outputs
    suite_dir = os.path.join(artifact_path, "experiments", dir_name)
    if os.path.exists(suite_dir) and not args.force:
        print(f"Error: Suite directory already exists: {suite_dir}", file=sys.stderr)
        print("Please provide a unique '--suite-dir-name' that hasn't been used, or pass --force to reuse it.")
        sys.exit(1)
    elif os.path.exists(suite_dir) and args.force:
        print(f"Using existing suite directory due to --force: {suite_dir}")

    os.makedirs(suite_dir, exist_ok=True)
    results_dir = os.path.join(suite_dir, "results")
    os.makedirs(results_dir, exist_ok=True)

    # Resolve configs and traces (using merged config names)
    configs, _origins = resolve_configs(data, all_cfg_names, yaml_dir)
    traces = resolve_traces(data, yaml_dir, trace_suite_name)

    # Write lists (CSV with headers)
    trace_list_path = os.path.join(suite_dir, "trace_list.csv")
    config_list_path = os.path.join(suite_dir, "config_list.csv")
    jobfile_path = os.path.join(suite_dir, "jobfile.sh")
    job_list_path = os.path.join(suite_dir, "job_list.csv")

    write_csv(
        trace_list_path,
        ["trace_name", "trace_path", "L2TLB_MPKI", "normalized_ptw_latency"],
        [(tname, tpath, mpki, nlat) for tname, tpath, mpki, nlat in traces],
    )
    write_csv(config_list_path, ["config_name", "config_flags"], [(cname, cflags.strip()) for cname, cflags in configs])

    sniper_path = os.path.join(artifact_path, "simulator", "sniper")
    jobs = build_jobfile(
        jobfile_path, sniper_path, instruction_count, configs, traces, results_dir,
        enable_icache_prefixes=args.enable_icache_for,
    )

    write_csv(job_list_path, ["job_id", "config_name", "trace_name", "output_dir"], jobs)

    print_header("Summary")
    print(f"  {Colors.BOLD}Suites:{Colors.RESET} {Colors.CYAN}{', '.join(suite_names)}{Colors.RESET} ({len(suite_names)} suite(s))")
    print(f"  {Colors.BOLD}Suite dir:{Colors.RESET} {Colors.CYAN}{suite_dir}{Colors.RESET} (exp_ enforced)")
    print(f"  {Colors.BOLD}Results dir:{Colors.RESET} {Colors.CYAN}{results_dir}{Colors.RESET}")
    print(f"  {Colors.BOLD}Instructions:{Colors.RESET} {Colors.GREEN}{instruction_count}{Colors.RESET}")
    print(f"  {Colors.BOLD}Traces:{Colors.RESET} {Colors.GREEN}{len(traces)}{Colors.RESET} -> {trace_list_path}")
    print(f"  {Colors.BOLD}Configs:{Colors.RESET} {Colors.GREEN}{len(configs)}{Colors.RESET} -> {config_list_path}")
    if args.enable_icache_for:
        print(f"  {Colors.BOLD}iCache for:{Colors.RESET} {Colors.YELLOW}{', '.join(args.enable_icache_for)}*{Colors.RESET} traces only")
    print(f"  {Colors.BOLD}Jobfile:{Colors.RESET} {jobfile_path}")
    print(f"  {Colors.BOLD}Jobs:{Colors.RESET} {Colors.GREEN}{len(jobs)}{Colors.RESET} -> {job_list_path}")


if __name__ == "__main__":
    main()

    