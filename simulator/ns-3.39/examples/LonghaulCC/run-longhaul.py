#!/usr/bin/env python3
"""Run one congestion-control algorithm and save its experiment data."""

import argparse
import subprocess
import sys
from pathlib import Path


#ALGORITHMS = ("dcqcn", "hpcc", "timely", "bifrost", "frp", "rocc", "proposed")
ALGORITHMS = ("dcqcn", "hpcc", "timely", "bifrost", "proposed")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--algorithm", required=True, choices=ALGORITHMS)
    args, rest = parser.parse_known_args()
    if "--algorithms" in rest:
        parser.error("use --algorithm to select one algorithm")
    runner = Path(__file__).with_name("run-longhaul-all.py")
    return subprocess.call([sys.executable, str(runner), *rest, "--algorithms", args.algorithm])


if __name__ == "__main__":
    raise SystemExit(main())
