#!/usr/bin/env python3
"""Run a matrix using one full config and explicit flow/stop-time overrides."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


DEFAULT_ALGORITHMS = ("dcqcn", "hpcc", "timely")


def git_commit(repo: Path) -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def flow_label(flow_path: Path) -> str:
    stem = flow_path.stem.lower()
    prefix = "flow-longhaul-"
    return stem[len(prefix):] if stem.startswith(prefix) else stem


def config_value(config_path: Path, key: str) -> str | None:
    for line in config_path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == key:
            return fields[1]
    return None


def render_config(config_path: Path, overrides: dict[str, object]) -> str:
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


def resolve_config_path(raw_path: str, ns3: Path) -> Path:
    path = Path(raw_path)
    return path.resolve() if path.is_absolute() else (ns3 / path).resolve()


def main() -> int:
    here = Path(__file__).resolve().parent
    ns3 = here.parents[1]
    repo = ns3.parents[1]
    default_config = here / "config-longhaul-common.txt"

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, default=repo / "results" / "longhaul")
    parser.add_argument("--config", type=Path, default=default_config,
                        help="full simulator config file")
    parser.add_argument("--flow-files", nargs="+", type=Path, default=None,
                        help="flow files to pass as --flow-file; omit to use FLOW_FILE in config")
    parser.add_argument("--stop-times", nargs="+", type=float, default=None,
                        help="one stop time for all flows, or one value per flow file")
    parser.add_argument("--algorithms", nargs="+", default=list(DEFAULT_ALGORITHMS),
                        help="values passed to the simulator's --cc option")
    parser.add_argument("--runs", type=int, default=1,
                        help="number of RngRun values, starting at 1")
    parser.add_argument("--seed", type=int, default=None,
                        help="override RNG_SEED; omit to use C++/config value")
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    args.output_root = args.output_root.resolve()
    args.config = args.config.resolve()
    if not args.config.is_file():
        parser.error("config file not found: " + str(args.config))
    if args.runs < 1:
        parser.error("--runs must be positive")

    config_flow = config_value(args.config, "FLOW_FILE")
    if not config_flow:
        parser.error(f"config has no FLOW_FILE: {args.config}")
    config_flow_path = resolve_config_path(config_flow, ns3)
    flow_paths = ([path.resolve() for path in args.flow_files]
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

    if not args.skip_build:
        subprocess.run([str(ns3 / "ns3"), "build", "longhaul-convergence", "-j2"],
                       cwd=ns3, check=True)
    binary = ns3 / "build" / "examples" / "LonghaulCC" / "ns3.39-longhaul-convergence-optimized"
    if not binary.is_file():
        raise SystemExit(f"simulator binary not found: {binary}; build it first")

    commit = git_commit(repo)
    args.output_root.mkdir(parents=True, exist_ok=True)
    failures = []
    source_config_hash = hashlib.sha256(args.config.read_bytes()).hexdigest()
    for flow_path, stop_time in zip(flow_paths, stop_times):
        label = flow_label(flow_path)
        for algorithm in args.algorithms:
            for run in range(1, args.runs + 1):
                seed_label = str(args.seed) if args.seed is not None else "config"
                run_dir = args.output_root / label / algorithm / f"seed{seed_label}-run{run}"
                run_dir.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(args.config, run_dir / "config.snapshot.txt")
                effective_config_path = run_dir / "config.txt"
                effective_config_text = render_config(args.config, {
                    "FCT_OUTPUT_FILE": run_dir / "fct.csv",
                    "PFC_OUTPUT_FILE": run_dir / "pfc.csv",
                    "RATE_OUTPUT_FILE": run_dir / "sender-rate.csv",
                    "GOODPUT_OUTPUT_FILE": run_dir / "receiver-goodput.csv",
                    "LINK_STATS_OUTPUT_FILE": run_dir / "dci-link.csv",
                    "RTT_OUTPUT_FILE": run_dir / "measured-rtt.csv",
                    "SUMMARY_META_FILE": run_dir / "metadata.json",
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
                    "status": status,
                    "error": error,
                    "wall_clock_seconds": wall_seconds,
                    "algorithm": simulator_meta.get("algorithm", algorithm),
                    "cc_mode": simulator_meta.get("cc_mode"),
                    "scenario": simulator_meta.get("scenario", label.upper()),
                    "seed": simulator_meta.get("rng_seed", args.seed),
                    "run": simulator_meta.get("rng_run", run),
                    "config_file": str(effective_config_path),
                    "config_sha256": effective_config_hash,
                    "source_config_sha256": source_config_hash,
                    "git_commit": commit,
                    "command": command,
                    "created_at_utc": datetime.now(timezone.utc).isoformat(),
                }
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
