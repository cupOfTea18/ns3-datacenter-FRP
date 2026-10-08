#!/usr/bin/env python3
"""Run one congestion-control algorithm and save its experiment data and metadata."""

import argparse
import csv
import hashlib
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


ALGORITHMS = ("dcqcn", "hpcc", "timely", "bifrost", "proposed")


def flow_label(flow_path):
    stem = flow_path.stem.lower()
    prefix = "flow-longhaul-"
    return stem[len(prefix):] if stem.startswith(prefix) else stem


def config_value(config_path, key):
    for line in config_path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == key:
            return fields[1]
    return None


def render_config(config_path, overrides):
    lines = []
    emitted = set()
    for line in config_path.read_text().splitlines():
        fields = line.split()
        if fields and fields[0] in overrides:
            lines.append(f"{fields[0]} {overrides[fields[0]]}")
            emitted.add(fields[0])
        else:
            lines.append(line)
    for key, value in overrides.items():
        if key not in emitted:
            lines.append(f"{key} {value}")
    return "\n".join(lines) + "\n"


def resolve_config_path(raw_path, ns3):
    path = Path(raw_path)
    return path.resolve() if path.is_absolute() else (ns3 / path).resolve()


def resolve_input_path(path, ns3):
    # Preserve paths relative to the caller; also accept ns-3 example paths.
    return path.resolve() if path.is_absolute() or path.is_file() else (ns3 / path).resolve()


def default_output_root(repo):
    name = "longhaul-" + datetime.now().strftime("%y%m%d-%H%M")
    root = repo / "results" / name
    suffix = 2
    while root.exists():
        root = repo / "results" / f"{name}-{suffix}"
        suffix += 1
    return root


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    here = Path(__file__).resolve().parent
    ns3 = here.parents[1]
    repo = ns3.parents[1]
    default_config = here / "config-longhaul.txt"

    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--output-root", type=Path, default=None,
                        help="result directory; default: results/longhaul-YYMMDD-HHMM")
    parser.add_argument("--config", type=Path, default=default_config,
                        help="config file; relative paths try the current directory, then ns-3 root")
    parser.add_argument("--flow-files", nargs="+", type=Path, default=None,
                        help="flow files; relative paths try the current directory, then ns-3 root; "
                             "omit to use FLOW_FILE in config")
    parser.add_argument("--stop-times", nargs="+", type=float, default=None,
                        help="one stop time for all flows, or one value per flow file")
    parser.add_argument("--algorithm", required=True, choices=ALGORITHMS,
                        help="value passed to the simulator's --cc option")
    parser.add_argument("--runs", type=int, default=1,
                        help="number of RngRun values, starting at 1")
    parser.add_argument("--seed", type=int, default=None,
                        help="override RNG_SEED; omit to use C++/config value")
    parser.add_argument("--purpose", choices=("transient", "completion"), default="transient",
                        help="fixed-duration transient observation or finite-flow completion validation")
    parser.add_argument("--r3-queue-mode", choices=("history", "snapshot"), default="history",
                        help="only replace R3 queue reconstruction; all other control mechanisms stay enabled")
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--skip-build", action="store_true",
                        help="skip compilation and use the existing simulator binary")
    args = parser.parse_args()

    args.output_root = (args.output_root or default_output_root(repo)).resolve()
    args.config = resolve_input_path(args.config, ns3)
    if not args.config.is_file():
        parser.error("config file not found: " + str(args.config))
    if args.runs < 1:
        parser.error("--runs must be positive")

    config_flow = config_value(args.config, "FLOW_FILE")
    if not config_flow:
        parser.error(f"config has no FLOW_FILE: {args.config}")
    config_flow_path = resolve_config_path(config_flow, ns3)
    flow_paths = ([resolve_input_path(path, ns3) for path in args.flow_files]
                  if args.flow_files is not None else [config_flow_path])
    missing_flows = [str(path) for path in flow_paths if not path.is_file()]
    if missing_flows:
        parser.error("flow file not found: " + ", ".join(missing_flows))

    if args.stop_times is None:
        stop_times = [None] * len(flow_paths)
    elif len(args.stop_times) == 1:
        stop_times = args.stop_times * len(flow_paths)
    elif len(args.stop_times) == len(flow_paths):
        stop_times = args.stop_times
    else:
        parser.error("--stop-times must contain one value or one value per --flow-files")

    if args.purpose == "completion" and args.stop_times is None:
        parser.error("completion experiments require an explicit common --stop-times")
    if any(t is not None and t <= 0 for t in stop_times):
        parser.error("stop times must be positive")
    scenarios = json.loads((here / "experiment-scenarios.json").read_text())
    config_topology = config_value(args.config, "TOPOLOGY_FILE")
    if not config_topology:
        parser.error(f"config has no TOPOLOGY_FILE: {args.config}")
    topology_source = resolve_config_path(config_topology, ns3)
    if not topology_source.is_file():
        parser.error("topology file not found: " + str(topology_source))
    algorithm = args.algorithm
    planned_dirs = set()
    for flow_path in flow_paths:
        for run in range(1, args.runs + 1):
            seed_label = str(args.seed) if args.seed is not None else "config"
            run_dir = args.output_root / flow_label(flow_path) / algorithm / f"seed{seed_label}-run{run}"
            if run_dir in planned_dirs:
                parser.error(f"multiple runs would use the same directory: {run_dir}")
            planned_dirs.add(run_dir)
            if run_dir.exists() and any(run_dir.iterdir()):
                parser.error(f"run directory already contains data: {run_dir}")

    print(f"Output directory: {args.output_root}", flush=True)
    if not args.skip_build:
        subprocess.run([str(ns3 / "ns3"), "build", "longhaul-convergence", "-j2"],
                       cwd=ns3, check=True)
    binary = ns3 / "build" / "examples" / "LonghaulCC" / "ns3.39-longhaul-convergence-optimized"
    if not binary.is_file():
        raise SystemExit(f"simulator binary not found: {binary}; build it first")

    args.output_root.mkdir(parents=True, exist_ok=True)
    failures = []
    source_config_hash = hashlib.sha256(args.config.read_bytes()).hexdigest()
    for flow_path, stop_time in zip(flow_paths, stop_times):
        label = flow_label(flow_path)
        for run in range(1, args.runs + 1):
            seed_label = str(args.seed) if args.seed is not None else "config"
            run_dir = args.output_root / label / algorithm / f"seed{seed_label}-run{run}"
            run_dir.mkdir(parents=True, exist_ok=True)
            effective_config_path = run_dir / "config.txt"
            effective_config_text = render_config(args.config, {
                "FLOW_FILE": flow_path,
                "TOPOLOGY_FILE": topology_source,
                "PROPOSED_RECONSTRUCT": int(args.r3_queue_mode == "history"),
                "FCT_OUTPUT_FILE": run_dir / "fct.csv",
                "PFC_OUTPUT_FILE": run_dir / "pfc.csv",
                "RATE_OUTPUT_FILE": run_dir / "sender-rate.csv",
                "GOODPUT_OUTPUT_FILE": run_dir / "receiver-goodput.csv",
                "LINK_STATS_OUTPUT_FILE": run_dir / "dci-link.csv",
                "RTT_OUTPUT_FILE": run_dir / "measured-rtt.csv",
                "SUMMARY_META_FILE": run_dir / "metadata.json",
                "PROPOSED_OUTPUT": run_dir / "r3",
            })
            effective_config_path.write_text(effective_config_text)
            effective_config_hash = hashlib.sha256(effective_config_text.encode()).hexdigest()

            command = [
                str(binary),
                f"--conf={effective_config_path}",
                f"--cc={algorithm}",
                f"--run={run}",
            ]
            if args.flow_files is not None:
                command.append(f"--flow-file={flow_path}")
            if args.seed is not None:
                command.append(f"--seed={args.seed}")
            if stop_time is not None:
                command.append(f"--stop-time={stop_time}")

            log_path = run_dir / "stdout.log"
            started = time.monotonic()
            status = "ok"
            return_code = 0
            error = ""
            try:
                with log_path.open("w") as log:
                    result = subprocess.run(
                        command,
                        cwd=ns3,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        timeout=args.timeout,
                        check=False,
                    )
                return_code = result.returncode
                if return_code != 0:
                    status = "failed"
                    error = f"simulator exited with {return_code}"
            except subprocess.TimeoutExpired:
                status = "timeout"
                return_code = -1
                error = f"timeout after {args.timeout}s"

            wall_seconds = time.monotonic() - started
            simulator_meta = {}
            metadata_path = run_dir / "metadata.json"
            if metadata_path.is_file():
                try:
                    simulator_meta = json.loads(metadata_path.read_text())
                except json.JSONDecodeError:
                    simulator_meta = {}
            run_metadata = {
                "purpose": args.purpose,
                "stop_rule": "fixed common simulator stop time; no algorithm-specific early selection",
                "scenario_contract": scenarios.get(label, {}),
                "r3_queue_mode": args.r3_queue_mode if algorithm == "proposed" else None,
                "flow_sha256": sha256(flow_path),
                "topology_sha256": sha256(topology_source),
                "original_flow_file": str(flow_path),
                "original_topology_file": str(topology_source),
                "status": status,
                "exit_code": return_code,
                "error": error,
                "wall_clock_seconds": wall_seconds,
                "algorithm": simulator_meta.get("algorithm", algorithm),
                "cc_mode": simulator_meta.get("cc_mode"),
                "proposed_version": (simulator_meta.get("proposed_parameters", {}).get("version")
                                     if algorithm == "proposed" else None),
                "scenario": simulator_meta.get("scenario", label.upper()),
                "seed": simulator_meta.get("rng_seed", args.seed if args.seed is not None
                                           else config_value(args.config, "RNG_SEED")),
                "run": simulator_meta.get("rng_run", run),
                "config_file": str(effective_config_path),
                "config_sha256": effective_config_hash,
                "source_config_sha256": source_config_hash,
                "command": command,
                "created_at_utc": datetime.now(timezone.utc).isoformat(),
            }
            common_summary = run_dir / "metadata.json.summary.json"
            if common_summary.is_file():
                summary = json.loads(common_summary.read_text())
                with (run_dir / "fct.csv").open() as stream:
                    fct_rows = list(csv.DictReader(stream))
                identities = {tuple(r[k] for k in ("src", "dst", "sport", "dport", "pg")) for r in fct_rows}
                run_metadata["completion_valid"] = (
                    summary["completed_flows"] == summary["expected_flows"] == len(fct_rows) == len(identities)
                    and all(f["rx_payload_bytes"] == f["size_bytes"] and f["unique_sent_bytes"] == f["size_bytes"]
                            for f in summary["flows"]))
                run_metadata["admission_drop_packets"] = summary["admission_drop_packets"]
            else:
                run_metadata["completion_valid"] = False
            if args.purpose == "completion" and not run_metadata["completion_valid"]:
                failures.append(str(run_dir) + " (incomplete)")
            (run_dir / "runner-metadata.json").write_text(
                json.dumps(run_metadata, indent=2) + "\n"
            )
            print(f"{status:7s} {label} {algorithm} seed={run_metadata['seed']} "
                  f"run={run_metadata['run']} ({wall_seconds:.2f}s)")
            if status != "ok":
                failures.append(str(run_dir))

    if failures:
        print("Failed runs:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
