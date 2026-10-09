#!/usr/bin/env python3
"""Run selected algorithms by invoking run-longhaul.py once per algorithm."""

import argparse
import subprocess
import sys
from datetime import datetime
from pathlib import Path


DEFAULT_ALGORITHMS = ("dcqcn", "hpcc", "timely", "bifrost", "proposed")


def default_output_root(repo):
    name = "longhaul-" + datetime.now().strftime("%y%m%d-%H%M")
    root = repo / "results" / name
    suffix = 2
    while root.exists():
        root = repo / "results" / f"{name}-{suffix}"
        suffix += 1
    return root


def main():
    here = Path(__file__).resolve().parent
    repo = here.parents[3]
    parser = argparse.ArgumentParser(
        description=__doc__, allow_abbrev=False,
        epilog="--config, --queue-mode and --service-mode (and other experiment options) are forwarded to run-longhaul.py.")
    parser.add_argument("--algorithms", nargs="+", choices=DEFAULT_ALGORITHMS,
                        default=list(DEFAULT_ALGORITHMS))
    parser.add_argument("--output-root", type=Path, default=None,
                        help="shared result directory; default: results/longhaul-YYMMDD-HHMM")
    parser.add_argument("--skip-build", action="store_true",
                        help="skip compilation for every algorithm and use the existing simulator binary")
    parser.add_argument("--algorithm", help=argparse.SUPPRESS)
    args, rest = parser.parse_known_args()
    if args.algorithm is not None:
        parser.error("use --algorithms to select algorithms")
    if len(set(args.algorithms)) != len(args.algorithms):
        parser.error("--algorithms must not contain duplicates")

    output_root = (args.output_root or default_output_root(repo)).resolve()
    runner = here / "run-longhaul.py"
    failures = []
    for index, algorithm in enumerate(args.algorithms):
        command = [sys.executable, str(runner), *rest,
                   "--output-root", str(output_root), "--algorithm", algorithm]
        if args.skip_build or index > 0:
            command.append("--skip-build")
        result = subprocess.run(command, check=False)
        if result.returncode != 0:
            failures.append(f"{algorithm} (exit {result.returncode})")

    if failures:
        print("Failed algorithms:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
