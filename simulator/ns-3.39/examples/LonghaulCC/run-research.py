#!/usr/bin/env python3
"""Small reproducible receiver-DCI incast experiments (no parameter sweep)."""
import argparse
import concurrent.futures
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
NS3 = HERE.parents[1]
REPO = NS3.parents[1]
VARIANTS = {
    'dcqcn': ('dcqcn', 0),
    'hpcc': ('hpcc', 0),
    'timely': ('timely', 0),
    'reactive-cnp': ('dcqcn', 1),
    'predictive-cnp': ('dcqcn', 2),
    'proposed': ('proposed', 2),
    'proposed-legacy': ('proposed-legacy', 2),
    'r1-reactive': ('proposed', 2),
    'r1-static': ('proposed', 2),
}


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=REPO / 'results/research-20260917/pilot')
    parser.add_argument('--variants', nargs='+', choices=VARIANTS, default=list(VARIANTS))
    parser.add_argument('--scenarios', nargs='+', choices=['single', 'join', 'finite'], default=['join', 'finite'])
    parser.add_argument('--stop', type=float, default=0.18)
    parser.add_argument('--wan-delay-us', type=float, default=5000)
    parser.add_argument('--no-window', action='store_true')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    inputs = root / 'inputs'
    inputs.mkdir(exist_ok=True)
    topology = inputs / 'topology.txt'
    topology.write_text('10 2 0 9\n8 9\n' + ''.join(f'{i} 8 100000000000 1.5us 0\n' for i in range(4)) + ''.join(f'9 {i} 100000000000 1.5us 0\n' for i in range(4, 8)) + f'8 9 200000000000 {args.wan_delay_us}us 0\n')
    (inputs / 'single.txt').write_text('1\n0 4 3 20000 2000000000 0.01\n')
    (inputs / 'join.txt').write_text('4\n' + ''.join(f'{i} 4 3 {20000+i} 2000000000 {0.01 if i == 0 else 0.05}\n' for i in range(4)))
    (inputs / 'finite.txt').write_text('4\n' + ''.join(f'{i} 4 3 {20000+i} 100000000 0.01\n' for i in range(4)))
    # Snapshot source evidence, including pre-existing local edits.
    (inputs / 'worktree.patch').write_bytes(subprocess.check_output(['git', 'diff'], cwd=REPO))
    for name in ['longhaul-convergence.cc', 'longhaul-research.h', 'longhaul-proposed-r1.h', 'run-research.py', 'config-longhaul-common.txt']:
        shutil.copy2(HERE / name, inputs / name)
    for name in ['rdma-hw.cc', 'switch-node.cc', 'switch-node.h']:
        shutil.copy2(NS3 / 'src/point-to-point/model' / name, inputs / name)
    binary = NS3 / 'build/examples/LonghaulCC/ns3.39-longhaul-convergence-optimized'
    source_config = HERE / 'config-longhaul-common.txt'
    manifest = {str(p.relative_to(inputs)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs.iterdir() if p.is_file()}
    manifest['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
    (inputs / 'sha256.json').write_text(json.dumps(manifest, indent=2))

    def run(task):
        scenario, variant = task
        out = root / scenario / variant
        out.mkdir(parents=True, exist_ok=True)
        if (out / 'run.json').exists():
            raise RuntimeError(f'Refusing to overwrite completed run: {out}')
        algorithm, control = VARIANTS[variant]
        config_path = out / 'config.txt'
        config_path.write_text(render_config(source_config, {
            'TOPOLOGY_FILE': topology,
            'DCI_LEFT': 8,
            'DCI_RIGHT': 9,
            'HAS_WIN': 0 if args.no_window else 1,
            'RESEARCH_RECEIVER': 4,
            'R1_PREDICTOR': {'r1-reactive': 1, 'r1-static': 2}.get(variant, 0),
            'RESEARCH_CONTROL': control,
            'RESEARCH_OUTPUT': out / 'bottleneck.csv',
            'FCT_OUTPUT_FILE': out / 'fct.csv',
            'PFC_OUTPUT_FILE': out / 'pfc.csv',
            'RATE_OUTPUT_FILE': out / 'sender-rate.csv',
            'GOODPUT_OUTPUT_FILE': out / 'receiver-goodput.csv',
            'LINK_STATS_OUTPUT_FILE': out / 'dci-link.csv',
            'RTT_OUTPUT_FILE': out / 'measured-rtt.csv',
            'SUMMARY_META_FILE': out / 'metadata.json',
            'RESEARCH_PERIOD': 0.0002 if variant in ('proposed', 'proposed-legacy') else 0.001,
            'RESEARCH_NEAR_PERIOD': 0.00005 if variant in ('proposed', 'proposed-legacy') else 0.0001,
            'RESEARCH_QREF': 250000 if variant in ('proposed', 'proposed-legacy') else 1000000,
            'RESEARCH_GUARDED': 1 if variant in ('proposed', 'proposed-legacy') else 0,
        }))
        command = [
            str(binary),
            f'--conf={config_path}',
            f'--flow-file={inputs / f"{scenario}.txt"}',
            f'--cc={algorithm}',
            f'--stop-time={args.stop}',
        ]
        started = time.monotonic()
        with (out / 'stdout.log').open('w') as log:
            try:
                result = subprocess.run(command, cwd=NS3, stdout=log, stderr=subprocess.STDOUT, timeout=600)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = -1
        record = dict(command=command, exit_code=code, wall_seconds=time.monotonic()-started, variant=variant, scenario=scenario)
        (out / 'run.json').write_text(json.dumps(record, indent=2))
        print(json.dumps(record), flush=True)
        return code

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        codes = list(pool.map(run, [(s, v) for s in args.scenarios for v in args.variants]))
    return int(any(codes))

if __name__ == '__main__':
    raise SystemExit(main())
