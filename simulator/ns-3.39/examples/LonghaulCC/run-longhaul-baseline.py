#!/usr/bin/env python3
"""Run reproducible long-haul baseline matrices.

The runner validates the topology, creates one isolated directory per
(scenario, algorithm, seed, run), snapshots the effective config, and keeps
stdout only as a diagnostic log.  The simulator writes all measurements to
CSV files in the same directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


ALGORITHMS = {"dcqcn": 1, "hpcc": 3, "timely": 7}
SCENARIOS = {
    "s0": ("flow-longhaul-s0.txt", 0.38),
    "s1": ("flow-longhaul-s1.txt", 0.38),
    "s2": ("flow-longhaul-s2.txt", 0.60),
    "s3": ("flow-longhaul-s3.txt", 0.60),
    "s4": ("flow-longhaul-s4.txt", 0.38),
    "s5": ("flow-longhaul-s5.txt", 0.38),
}


def replace_config(template: str, values: dict[str, str]) -> str:
    lines = []
    emitted = set()
    for line in template.splitlines():
        fields = line.split()
        if fields and fields[0] in values:
            lines.append(f"{fields[0]} {values[fields[0]]}")
            emitted.add(fields[0])
        else:
            lines.append(line)
    for key, value in values.items():
        if key not in emitted:
            lines.append(f"{key} {value}")
    return "\n".join(lines) + "\n"


def git_commit(repo: Path) -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> int:
    here = Path(__file__).resolve().parent
    ns3 = here.parents[1]
    repo = ns3.parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, default=repo / "results" / "longhaul")
    parser.add_argument("--scenarios", nargs="+", choices=sorted(SCENARIOS), default=["s0", "s2"])
    parser.add_argument("--algorithms", nargs="+", choices=sorted(ALGORITHMS), default=list(ALGORITHMS))
    parser.add_argument("--runs", type=int, default=1, help="number of RngRun values, starting at 1")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--stop-time", type=float, default=None,
                        help="override scenario stop time (useful only for a short startup smoke test)")
    parser.add_argument("--full", action="store_true", help="run S0..S5 and five runs")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()
    args.output_root = args.output_root.resolve()
    if args.full:
        args.scenarios = list(SCENARIOS)
        if args.runs == 1:
            args.runs = 5
    if args.runs < 1:
        parser.error("--runs must be positive")

    topology = here / "topology-longhaul.txt"
    template_path = here / "config-longhaul-common.txt"
    validator = here / "validate_longhaul_topology.py"
    subprocess.run([sys.executable, str(validator), str(topology)], check=True)
    if not args.skip_build:
        subprocess.run([str(ns3 / "ns3"), "build", "longhaul-convergence", "-j2"], cwd=ns3, check=True)
    binary = ns3 / "build" / "examples" / "LonghaulCC" / "ns3.39-longhaul-convergence-optimized"
    if not binary.is_file():
        raise SystemExit(f"simulator binary not found: {binary}; build it first")

    template = template_path.read_text()
    commit = git_commit(repo)
    args.output_root.mkdir(parents=True, exist_ok=True)
    failures = []
    for scenario in args.scenarios:
        flow_name, scenario_stop_time = SCENARIOS[scenario]
        stop_time = args.stop_time if args.stop_time is not None else scenario_stop_time
        flow_path = here / flow_name
        for algorithm in args.algorithms:
            for run in range(1, args.runs + 1):
                run_dir = args.output_root / scenario / algorithm / f"seed{args.seed}-run{run}"
                run_dir.mkdir(parents=True, exist_ok=True)
                config_path = run_dir / "config.snapshot.txt"
                values = {
                    "TOPOLOGY_FILE": str(topology),
                    "FLOW_FILE": str(flow_path),
                    "CC_MODE": str(ALGORITHMS[algorithm]),
                    "SIMULATOR_STOP_TIME": str(stop_time),
                    "RNG_SEED": str(args.seed),
                    "RNG_RUN": str(run),
                    "SCENARIO": scenario.upper(),
                    "FCT_OUTPUT_FILE": str(run_dir / "fct.csv"),
                    "PFC_OUTPUT_FILE": str(run_dir / "pfc.csv"),
                    "RATE_OUTPUT_FILE": str(run_dir / "sender-rate.csv"),
                    "GOODPUT_OUTPUT_FILE": str(run_dir / "receiver-goodput.csv"),
                    "LINK_STATS_OUTPUT_FILE": str(run_dir / "dci-link.csv"),
                    "SUMMARY_META_FILE": str(run_dir / "metadata.json"),
                }
                config_text = replace_config(template, values)
                config_path.write_text(config_text)
                config_hash = hashlib.sha256(config_text.encode()).hexdigest()
                log_path = run_dir / "stdout.log"
                started = time.monotonic()
                status = "ok"
                return_code = 0
                error = ""
                try:
                    with log_path.open("w") as log:
                        result = subprocess.run(
                            [str(binary), f"--conf={config_path}"],
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
                run_metadata = {
                    "status": status,
                    "error": error,
                    "wall_clock_seconds": wall_seconds,
                    "algorithm": algorithm,
                    "cc_mode": ALGORITHMS[algorithm],
                    "scenario": scenario.upper(),
                    "seed": args.seed,
                    "run": run,
                    "git_commit": commit,
                    "config_sha256": config_hash,
                    "command": [str(binary), f"--conf={config_path}"],
                    "created_at_utc": datetime.now(timezone.utc).isoformat(),
                }
                (run_dir / "runner-metadata.json").write_text(json.dumps(run_metadata, indent=2) + "\n")
                print(f"{status:7s} {scenario} {algorithm} seed={args.seed} run={run} ({wall_seconds:.2f}s)")
                if status != "ok":
                    failures.append(str(run_dir))
    if failures:
        print("Failed runs:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
