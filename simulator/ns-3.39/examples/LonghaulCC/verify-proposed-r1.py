#!/usr/bin/env python3
"""Verify R1 byte conservation and packet-level token eligibility from a run folder."""
import argparse
import csv
import json
from pathlib import Path


def rows(path):
    with path.open() as f:
        yield from csv.DictReader(f)


def verify(folder):
    meta = json.loads((folder / 'metadata.json').read_text())
    params = meta['r1_parameters']
    assert params['enabled'], 'requires a Proposed-R1 run'
    control = list(rows(folder / 'bottleneck.csv'))
    receiver = list(rows(folder / 'bottleneck.csv.receiver.csv'))
    updates = [(float(r['time_s']), float(r['value']) / 8)
               for r in rows(folder / 'bottleneck.csv.events.csv')
               if r['event'] == 'TARGET_APPLIED_BPS']
    assert updates and updates[0][0] == 0
    burst = params['burst_bytes']
    tokens, last, rate, index, sent = float(burst), 0.0, updates[0][1], 1, 0
    for r in rows(folder / 'bottleneck.csv.packets.csv'):
        now, size = float(r['time_s']), int(r['bytes'])
        while index < len(updates) and updates[index][0] <= now:
            t, new_rate = updates[index]
            tokens = min(burst, tokens + rate * (t-last))
            last, rate = t, new_rate
            index += 1
        tokens = min(burst, tokens + rate * (now-last))
        # ns-3 rounds scheduling to ns; the device permits <= 1 ns timing error.
        tolerance = rate * 1e-9 + 1
        assert rate > 0 and tokens + tolerance >= size, (now, size, tokens, rate)
        tokens = max(0.0, tokens-size)
        last = now
        sent += size
    for r in control:
        assert int(r['source_in_bytes']) - int(r['source_tx_bytes']) == int(r['source_queue_bytes'])
    for r in receiver:
        assert int(r['enqueued_bytes']) - int(r['departed_bytes']) == int(r['queue_bytes'])
    final = control[-1]
    assert sent == int(final['source_tx_bytes']), 'log must cover all transmitted packets'
    fct = list(rows(folder / 'fct.csv'))
    errors = [float(r['value']) for r in rows(folder / 'bottleneck.csv.events.csv')
              if r['event'] == 'PREDICTION_ERROR_BYTES']
    def area(data, key):
        previous, total = 0.0, 0.0
        for r in data:
            now = float(r['time_s'])
            total += float(r[key]) * (now-previous)
            previous = now
        return total  # sampled rectangle integral, byte seconds
    result = {
        'packet_gate_verified': True,
        'completed_flows': len(fct),
        'max_fct_ms': max((int(r['fct_ns'])/1e6 for r in fct), default=None),
        'source_bytes_in': int(final['source_in_bytes']),
        'source_bytes_tx': sent,
        'receiver_bytes_in': int(receiver[-1]['enqueued_bytes']),
        'receiver_bytes_out': int(receiver[-1]['departed_bytes']),
        'source_sampled_peak_bytes': max(int(r['source_queue_bytes']) for r in control),
        'receiver_event_peak_bytes': max(int(r['interval_peak_bytes']) for r in receiver),
        'network_sampled_peak_bytes': max(int(r['network_queue_bytes']) for r in control),
        'source_sampled_area_byte_seconds': area(control, 'source_queue_bytes'),
        'receiver_sampled_area_byte_seconds': area(receiver, 'queue_bytes'),
        'network_sampled_area_byte_seconds': area(control, 'network_queue_bytes'),
        'cnp_sent': int(final['cnp_sent']),
        'admission_drops': int(final['admission_drop_packets']),
        'control_tx_bytes': int(final['control_tx_bytes']),
        'modes': sorted({r['mode'] for r in control}),
        'prediction_pairs': len(errors),
        'prediction_max_underestimate_bytes': max([0.0] + errors),
        'rejected_reports': int(final['rejected_reports']),
    }
    print(json.dumps(result, indent=2))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    verify(args.folder)
