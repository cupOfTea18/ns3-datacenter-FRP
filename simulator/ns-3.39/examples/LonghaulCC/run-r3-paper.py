#!/usr/bin/env python3
"""Run the fixed R3 paper matrix through the existing single-algorithm runner."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from zoneinfo import ZoneInfo

HERE = Path(__file__).resolve().parent
NS3 = HERE.parent.parent
REPO = NS3.parent.parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--phase', choices=('core', 'delay'), required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    root = args.output_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    plan_path = root / (args.phase + '-manifest.json')
    if plan_path.exists():
        parser.error('phase already exists; use a new root to repeat an experiment')
    if args.phase == 'delay':
        core = json.loads((root / 'core-results.json').read_text())
        if not all(x['valid'] for x in core):
            parser.error('core integrity gate failed; inspect core-results.json first')
    subprocess.run([str(NS3 / 'ns3'), 'build', 'longhaul-convergence', '-j2'],
                   cwd=NS3, check=True)
    spec = importlib.util.spec_from_file_location('runner', HERE / 'run-longhaul.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    binary = NS3 / 'build/examples/LonghaulCC/ns3.39-longhaul-convergence-optimized'
    source = HERE / 'config-longhaul.txt'
    topology = HERE / 'topology-longhaul.txt'
    inputs = root / 'inputs'
    inputs.mkdir(exist_ok=True)
    variants = [('dcqcn', 'dcqcn', 'history', 1),
                ('default-history', 'proposed', 'history', 1),
                ('default-snapshot', 'proposed', 'snapshot', 1),
                ('candidate-history', 'proposed', 'history', 0),
                ('candidate-snapshot', 'proposed', 'snapshot', 0)]
    matrix = []
    delays = [5] if args.phase == 'core' else [1, 10]
    for delay in delays:
        topo = inputs / f'topology-{delay}ms.txt'
        text = topology.read_text()
        old = '40 81 200000000000.0 5ms 0'
        assert text.count(old) == 1
        topo.write_text(text.replace(old, f'40 81 200000000000.0 {delay}ms 0'))
        for label, algorithm, mode, ecn in variants:
            if args.phase == 'delay' and label.startswith('default-'):
                continue
            conf = inputs / f'config-{delay}ms-ecn{ecn}.txt'
            conf.write_text(runner.render_config(source, {'TOPOLOGY_FILE': topo,
                                                          'PROPOSED_SHAPER_ECN': ecn}))
            for seed in (1, 2, 3):
                matrix.append(dict(label=label, algorithm=algorithm, mode=mode,
                                   ecn=ecn, seed=seed, delay_ms=delay, config=str(conf),
                                   flow=str(HERE / 'flow-longhaul-r3-mechanism.txt'),
                                   control=False))
        if args.phase == 'core':
            for ecn, label in ((1, 'default-control'), (0, 'candidate-control')):
                matrix.append(dict(label=label, algorithm='proposed', mode='history',
                                   ecn=ecn, seed=1, delay_ms=5,
                                   config=str(inputs / f'config-5ms-ecn{ecn}.txt'),
                                   flow=str(HERE / 'flow-longhaul-r3-mechanism-control.txt'),
                                   control=True))
    for item in matrix:
        output = root / f'delay-{item["delay_ms"]}ms' / item['label']
        item['command'] = [sys.executable, str(HERE / 'run-longhaul.py'),
                           '--algorithm', item['algorithm'], '--config', item['config'],
                           '--flow-files', item['flow'], '--seed', str(item['seed']),
                           '--r3-queue-mode', item['mode'], '--purpose', 'completion',
                           '--stop-times', '8', '--timeout', '600', '--skip-build',
                           '--output-root', str(output)]
        item['run_dir'] = str(output / runner.flow_label(Path(item['flow'])) /
                              item['algorithm'] / f'seed{item["seed"]}-run1')
    manifest = dict(created_at=datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
                    git_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'],
                                                     cwd=REPO, text=True).strip(),
                    binary_sha256=digest(binary), config_sha256=digest(source),
                    stop_time_s=8, seeds=[1, 2, 3],
                    source_hashes={str(p.relative_to(REPO)): digest(p) for p in
                                   [HERE / 'longhaul-r3.h', HERE / 'longhaul-convergence.cc',
                                    NS3 / 'src/point-to-point/model/dci-gateway-node.cc',
                                    Path(__file__)]}, experiments=matrix)
    plan_path.write_text(json.dumps(manifest, indent=2) + '\n')

    def run(item):
        log = root / f'{args.phase}-{item["label"]}-{item["delay_ms"]}ms-seed{item["seed"]}.log'
        with log.open('w') as stream:
            result = subprocess.run(item['command'], cwd=NS3, stdout=stream,
                                    stderr=subprocess.STDOUT)
        run_dir = Path(item['run_dir'])
        valid = False
        evidence = {}
        if (run_dir / 'metadata.json.summary.json').exists():
            summary = json.loads((run_dir / 'metadata.json.summary.json').read_text())
            meta = json.loads((run_dir / 'runner-metadata.json').read_text())
            evidence = {k: summary[k] for k in ('expected_flows', 'completed_flows',
                        'admission_drop_packets', 'remaining_switch_queue_bytes',
                        'remaining_mmu_bytes', 'remaining_mmu_egress_bytes')}
            evidence['retransmitted_payload_bytes'] = sum(
                f['tx_payload_bytes'] - f['unique_sent_bytes'] for f in summary['flows'])
            valid = (result.returncode == 0 and meta['completion_valid'] and
                     all(evidence[k] == 0 for k in evidence if k not in
                         ('expected_flows', 'completed_flows')))
        row = {**item, 'returncode': result.returncode, 'valid': valid, **evidence}
        print(f'{item["label"]} delay={item["delay_ms"]}ms seed={item["seed"]}: '
              f'valid={valid} {evidence}', flush=True)
        return row

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in as_completed([pool.submit(run, item) for item in matrix]):
            results.append(future.result())
            (root / (args.phase + '-results.json')).write_text(
                json.dumps(results, indent=2) + '\n')
    assert digest(binary) == manifest['binary_sha256'], 'binary changed during experiments'
    return 0 if all(x['valid'] for x in results) else 1


if __name__ == '__main__':
    sys.exit(main())
