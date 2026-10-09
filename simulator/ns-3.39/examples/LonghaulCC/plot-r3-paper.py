#!/usr/bin/env python3
"""Audit and plot the fixed matrix produced by run-r3-paper.py (no simulation)."""
import argparse
import csv
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

LABELS = ['dcqcn', 'default-history', 'default-snapshot',
          'candidate-history', 'candidate-snapshot']
SHORT = ['DCQCN', 'ECN on\nhistory', 'ECN on\nsnapshot',
         'ECN off\nhistory', 'ECN off\nsnapshot']
COLORS = ['#555555', '#b64236', '#d89648', '#176d9b', '#21876a']
plt.rcParams.update({'font.size': 10, 'axes.spines.top': False,
                     'axes.spines.right': False, 'pdf.fonttype': 42})


def rows(path):
    with path.open() as stream:
        yield from csv.DictReader(stream)


def write_csv(path, data):
    with path.open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(data[0]))
        writer.writeheader()
        writer.writerows(data)


def save(fig, out, name):
    fig.savefig(out / (name + '.png'), dpi=200, bbox_inches='tight')
    fig.savefig(out / (name + '.pdf'), bbox_inches='tight')
    plt.close(fig)
    print('saved', name, flush=True)


def gateway(run, node, stop=.12):
    result = {1: [], 2: []}
    for r in rows(run / f'r3.gateway-{node}.csv'):
        if float(r['time_s']) > stop:
            break
        result[int(r['flow'])].append({k: float(v) for k, v in r.items()
                                      if k != 'role'})
    return result


def metric_array(series, key):
    return np.array([r[key] for r in series])


def packets(run, node, stop=.12):
    data = []
    for r in rows(run / f'r3.gateway-{node}.packets.csv'):
        if int(r['time_ns']) > round(stop * 1e9):
            break
        data.append([int(r[k]) for k in ('time_ns', 'flow', 'bytes')])
    return np.asarray(data, dtype=np.int64)


def statistics(item):
    run = Path(item['run_dir'])
    summary = json.loads((run / 'metadata.json.summary.json').read_text())
    fcts = list(rows(run / 'fct.csv'))
    end = max((int(r['start_time_ns']) + int(r['fct_ns'])) / 1e9 for r in fcts)
    result = {k: item[k] for k in ('label', 'seed', 'delay_ms', 'control', 'valid', 'run_dir')}
    for src, dst in ((0, 41), (0, 59), (42, 41)):
        found = [r for r in fcts if int(r['src']) == src and int(r['dst']) == dst]
        result[f'fct_{src}_{dst}_ms'] = int(found[0]['fct_ns']) / 1e6 if found else ''
    result['drops'] = summary['admission_drop_packets']
    result['pfc_trace_events'] = sum(1 for _ in rows(run / 'pfc.csv'))
    peak = 0
    for r in rows(run / 'metadata.json.queues.csv'):
        if int(r['time_ns']) > end * 1e9:
            break
        if int(r['node']) == 81 and int(r['peer']) == 79 and int(r['pg']) == 3:
            peak = max(peak, int(r['queue_bytes']))
    result['b_egress_sampled_peak_bytes'] = peak
    control_bytes = 0
    endpoints = {}
    if item['algorithm'] == 'proposed':
        for node in (40, 81):
            last = {}
            before = 0
            during = 0
            for r in rows(run / f'r3.gateway-{node}.csv'):
                t = float(r['time_s'])
                last[int(r['flow'])] = r
                if t <= .01:
                    before = int(r['control_tx_bytes'])
                if t <= end:
                    during = int(r['control_tx_bytes'])
            control_bytes += during - before
            endpoints[node] = last
        for fid in (1, 2):
            a, b = endpoints[40][fid], endpoints[81][fid]
            assert int(a['in_bytes']) == int(a['tx_bytes']) == int(b['in_bytes']) == int(b['tx_bytes'])
            assert int(a['queue_bytes']) == int(b['queue_bytes']) == 0
        for node in endpoints:
            assert all(int(r['rejected']) == 0 for r in endpoints[node].values())
        data_bytes = sum(int(r['tx_bytes']) for r in endpoints[40].values())
    else:
        data_bytes = 0
    result['gateway_wire_closed'] = bool(endpoints) if item['algorithm'] == 'proposed' else 'not instrumented'
    result['control_tx_bytes_active'] = control_bytes
    result['control_to_A_data_percent'] = 100 * control_bytes / data_bytes if data_bytes else 0
    result['active_end_s'] = end
    result['common_supply_ms'] = ((min(f['supply_end_ns'] for f in summary['flows'])/1e9-.03)*1000
                                  if not item['control'] else '')
    result['required_hold_ms'] = max(20., 3*max(int(r['base_rtt_ns']) for r in fcts)/1e6)
    result['supply_hold_met'] = (result['common_supply_ms'] >= result['required_hold_ms']
                               if not item['control'] else 'not applicable')
    return result


def topology_figure(run, out):
    meta = json.loads((run / 'metadata.json').read_text())
    positions = {0: (0, 1), 32: (1, 1), 39: (2, 1.6), 36: (2, .4),
                 40: (3, 1), 81: (5, 1), 79: (6, 1), 73: (7, 1.6),
                 75: (7, .4), 41: (8, 1.6), 59: (8, .4), 42: (6, 2.3)}
    fig, ax = plt.subplots(figsize=(12, 4))
    for i, flow in enumerate(meta['flow_path_metrics']):
        for u, v, _ in flow['data_path']:
            ax.annotate('', positions[v], positions[u], arrowprops=dict(
                arrowstyle='->', color=['#b64236', '#176d9b', '#21876a'][i],
                lw=2, connectionstyle=f'arc3,rad={.06 * (i - 1)}', shrinkA=13, shrinkB=13))
    for node, (x, y) in positions.items():
        name = f'{node}' if node not in (40, 81) else ('A / 40' if node == 40 else 'B / 81')
        ax.text(x, y, name, ha='center', va='center', bbox=dict(boxstyle='round',
                fc='#eef1f4', ec='#677988'), fontsize=10)
    ax.text(4, 1.23, '200 Gb/s, 5 ms', ha='center')
    ax.text(4, .8, 'DCI', ha='center')
    ax.text(4, -.05, 'Other links: 100 Gb/s, 1.5 us; full 82-node topology retained', ha='center')
    ax.text(6.8, 2.65, 'Local competitor: 42 -> 41, starts at 30 ms', ha='center', color='#21876a')
    ax.text(1.3, 2.35, 'F1: 0 -> 41 (red)\nF2: 0 -> 59 (blue)\nBoth start at 10 ms', ha='center')
    ax.set(xlim=(-.4, 8.5), ylim=(-.2, 2.95), title='Observed data paths, seed 1 (unused nodes omitted)')
    ax.axis('off')
    save(fig, out, 'fig01-paths')


def timeline_figure(out):
    fig, ax = plt.subplots(figsize=(10, 3.2))
    # Dimensionless positions: explanatory schematic, not measured times.
    for y, name in ((1, 'A: actual departures'), (0, 'B: queue evolution')):
        ax.annotate('', (4.3, y), (-.2, y), arrowprops=dict(arrowstyle='->', color='#555555'))
        ax.text(-.3, y, name, ha='right', va='center')
    ax.plot([0, 2.8], [1, 1], lw=5, color='#176d9b')
    ax.plot([1, 3.8], [0, 0], lw=5, color='#21876a')
    for x, y, label in ((0, 1, r'$t_s-d_f$'), (2.8, 1, r'$t$'),
                        (1, 0, r'$t_s$'), (3.8, 0, r'$t+d_f$')):
        ax.plot(x, y, 'o', color='#333333'); ax.text(x, y+.18, label, ha='center')
    for a, b in ((0, 1), (2.8, 3.8)):
        ax.annotate('', (b, .05), (a, .93), arrowprops=dict(arrowstyle='->', ls='--', color='#888888'))
    ax.text(1.4, 1.48, 'Known input: replay bytes actually sent', ha='center')
    ax.text(2.4, -.5, 'Start from snapshot; estimate service until action arrival', ha='center')
    ax.set(xlim=(-1.7, 4.5), ylim=(-.65, 1.7), title='Time alignment (schematic, not an experimental result)')
    ax.axis('off'); save(fig, out, 'fig02-time-alignment')


def reaction_figure(run, control, out):
    fig, axes = plt.subplots(2, 2, figsize=(11, 7), layout='constrained')
    for current, style, suffix in ((run, '-', 'competition'), (control, '--', 'control')):
        b = gateway(current, 81)
        for fid, color in ((1, '#b64236'), (2, '#176d9b')):
            series = b[fid]
            t = metric_array(series, 'time_s') * 1000
            for ax, key in ((axes[0, 0], 'reaction_bps'), (axes[0, 1], 'budget_bps')):
                ax.plot(t, metric_array(series, key)/1e9, style, color=color,
                        label=f'F{fid}, {suffix}', alpha=.9)
        pk = packets(current, 81)
        edges = np.arange(.01, .121, .001)
        for fid, color in ((1, '#b64236'), (2, '#176d9b')):
            mask = pk[:, 1] == fid
            counts = np.histogram(pk[mask, 0] / 1e9, edges, weights=pk[mask, 2])[0]
            axes[1, 0].step(edges[1:]*1000, counts * 8 / .001 / 1e9,
                            where='pre', ls=style, color=color, label=f'F{fid}, {suffix}')
        gp = {(0, 41): [], (0, 59): [], (42, 41): []}
        for r in rows(current / 'receiver-goodput.csv'):
            if int(r['time_ns']) > 120000000: break
            gp[(int(r['src']), int(r['dst']))].append(r)
        for pair, rr in gp.items():
            if not rr: continue
            fid = {(0, 41): 1, (0, 59): 2, (42, 41): 3}[pair]
            # Aggregate the native 100 us byte counts into fixed 1 ms bins.
            times = [(int(r['interval_start_ns'])+int(r['time_ns']))/2e9 for r in rr]
            counts = np.histogram(times, edges, weights=[int(r['goodput_bytes']) for r in rr])[0]
            axes[1, 1].step(edges[1:]*1000, counts*8/.001/1e9, where='pre', ls=style,
                            color=['#b64236', '#176d9b', '#21876a'][fid-1], label=f'F{fid}, {suffix}')
    for ax, title in zip(axes.flat, ['B reaction ceiling', 'B allocated budget',
                                    'B actual wire TX (1 ms bins)', 'Receiver payload goodput (1 ms bins)']):
        ax.set(title=title, xlabel='Time (ms)', ylabel='Gb/s', xlim=(10, 120))
        ax.axvline(30, color='k', ls=':', lw=1); ax.grid(alpha=.2); ax.legend(fontsize=7)
    fig.suptitle('R3 history, shaper ECN off, seed 1; dashed: no local competitor')
    save(fig, out, 'fig03-flow-reaction')


def prediction_figure(run, out):
    a, b = gateway(run, 40), gateway(run, 81)
    ap, bp = packets(run, 40), packets(run, 81)
    samples = a[1]
    delays = [round((r['prediction_end_s']-r['time_s'])*1e9) for r in samples]
    assert len(set(delays)) == 1 and len(set(ap[:, 2])) == 1
    arrivals = ap[:, 0] + delays[0]
    departures = bp[:, 0]
    ca = np.r_[0, np.cumsum(ap[:, 2])]
    cb = np.r_[0, np.cumsum(bp[:, 2])]
    def incoming(t): return ca[np.searchsorted(arrivals, t, side='right')]
    def outgoing(t): return cb[np.searchsorted(departures, t, side='right')]
    # A tick and a packet event can share a nanosecond but have different event IDs.
    # Validate each counter against the before/after envelope at that nanosecond.
    # Snapshot counters below preserve the actual ordering; endpoints use after-all.
    bt = np.rint(metric_array(b[1], 'time_s')*1e9).astype(np.int64)
    bi = metric_array(b[1], 'in_bytes') + metric_array(b[2], 'in_bytes')
    bo = metric_array(b[1], 'tx_bytes') + metric_array(b[2], 'tx_bytes')
    left_in = ca[np.searchsorted(arrivals, bt, side='left')]
    left_out = cb[np.searchsorted(departures, bt, side='left')]
    assert np.all((left_in <= bi) & (bi <= incoming(bt)))
    assert np.all((left_out <= bo) & (bo <= outgoing(bt)))
    snapshot_counters = {int(t): (int(i), int(o)) for t, i, o in zip(bt, bi, bo)}
    alignment_error = np.max(np.abs(incoming(bt)-bi))
    departure_error = np.max(np.abs(outgoing(bt)-bo))
    evidence = []
    for r in samples:
        if not (.03 <= r['time_s'] < .08 and r['snapshot_valid'] == 1): continue
        ts = round(r['snapshot_time_s']*1e9)
        end = round(r['prediction_end_s']*1e9)
        q0 = r['snapshot_queue_bytes']
        snapshot_in, snapshot_out = snapshot_counters[ts]
        assert snapshot_in - snapshot_out == q0
        lo = np.searchsorted(ca, snapshot_in)
        assert ca[lo] == snapshot_in
        hi = np.searchsorted(arrivals, end, side='right')
        service = r['service_bps']/8
        total = ca[hi]-ca[lo]
        qend = q0 + total - service*(end-ts)*1e-9
        before_arrivals = q0+(ca[lo:hi]-ca[lo])-service*(arrivals[lo:hi]-ts)*1e-9
        packet_replay = qend-min(0., qend, float(before_arrivals.min()) if hi>lo else 0.)
        actual = incoming(end)-outgoing(end)
        oracle = q0 + incoming(end)-snapshot_in - (outgoing(end)-snapshot_out)
        assert oracle == actual and actual >= 0
        evidence.append(dict(time_ms=r['time_s']*1000, endpoint_ns=end,
            snapshot_queue_bytes=q0, predicted_queue_bytes=r['predicted_queue_bytes'],
            constant_service_packet_replay_bytes=packet_replay, actual_queue_bytes=int(actual),
            offline_departure_oracle_bytes=int(oracle), service_estimate_gbps=service*8/1e9,
            realized_service_gbps=(outgoing(end)-snapshot_out)*8/(end-ts),
            A_queue_bytes=0))
    # Match A samples by time, rather than relying on the filtered sample offset.
    qA = {round(r['time_s']*1e9): r['queue_bytes'] for r in a[2]}
    sampleA = {round(r['time_s']*1e9): r for r in a[1]}
    for row in evidence:
        t = round(row['time_ms']*1e6)
        row['A_queue_bytes'] = qA[t] + sampleA[t]['queue_bytes']
    write_csv(out / 'prediction-replay.csv', evidence)
    t = np.array([r['time_ms'] for r in evidence])
    def val(k): return np.array([r[k] for r in evidence])
    fig, axes = plt.subplots(2, 2, figsize=(11, 7), layout='constrained')
    for key, label, style in [('predicted_queue_bytes', 'Online bucket reconstruction', '-'),
        ('constant_service_packet_replay_bytes', 'Exact arrivals + frozen service', '--'),
        ('snapshot_queue_bytes', 'Old snapshot', ':'),
        ('actual_queue_bytes', 'B queue at exact action time', '-.')]:
        axes[0, 0].plot(t, val(key)/1e6, style, label=label)
    axes[0, 0].set(ylabel='MB', title='Queue estimate at action arrival')
    axes[0, 1].plot(t, val('actual_queue_bytes')/1e3, label='Event-reconstructed B queue')
    axes[0, 1].plot(t, val('offline_departure_oracle_bytes')/1e3, '--', label='Offline departure oracle (overlaps)')
    axes[0, 1].set(ylabel='kB', title='Actual queue / oracle (zoomed scale)')
    for key, label in [('predicted_queue_bytes', 'Online history'),
                       ('snapshot_queue_bytes', 'Old snapshot')]:
        axes[1, 0].plot(t, (val(key)-val('actual_queue_bytes'))/1e6, label=label)
    axes[1, 0].set(ylabel='Signed error (MB)', title='Estimator error on the same history trajectory')
    for key, label in [('service_estimate_gbps', 'Snapshot service estimate'),
                       ('realized_service_gbps', 'Realized B departures / horizon')]:
        axes[1, 1].plot(t, val(key), label=label)
    axes[1, 1].set(ylabel='Gb/s', title='Service assumption versus realized departures')
    for ax in axes.flat:
        ax.set(xlabel='A decision time (ms)', xlim=(30, 80)); ax.grid(alpha=.2); ax.legend(fontsize=7)
    fig.suptitle('R3 history, shaper ECN off, seed 1; oracle uses future departures, not deployable')
    save(fig, out, 'fig04-prediction')
    metrics = {'samples':len(evidence), 'counter_event_envelope_violations':0,
               'B_validation_samples':len(bt),
               'arrival_after_all_tie_difference_max_bytes':int(alignment_error),
               'departure_after_all_tie_difference_max_bytes':int(departure_error),
               'arrival_tie_samples':int(np.count_nonzero(left_in != incoming(bt))),
               'departure_tie_samples':int(np.count_nonzero(left_out != outgoing(bt))),
               'endpoint_convention':'after all packet events at endpoint nanosecond; snapshots use logged counters'}
    for key in ('predicted_queue_bytes', 'snapshot_queue_bytes', 'constant_service_packet_replay_bytes'):
        error = np.abs(val(key)-val('actual_queue_bytes'))
        metrics[key+'_mae'] = float(error.mean())
        metrics[key+'_max_error'] = float(error.max())
        mask = val('A_queue_bytes') > 0
        metrics[key+'_backlogged_mae'] = float(error[mask].mean()) if mask.any() else None
    metrics['backlogged_samples'] = int((val('A_queue_bytes') > 0).sum())
    (out / 'prediction-metrics.json').write_text(json.dumps(metrics, indent=2)+'\n')


def ablation_figure(stats, out):
    fig, axes = plt.subplots(2, 3, figsize=(13, 7), layout='constrained')
    keys = ['fct_0_41_ms', 'fct_0_59_ms', 'fct_42_41_ms',
            'b_egress_sampled_peak_bytes', 'pfc_trace_events', 'control_to_A_data_percent']
    titles = ['F1: affected cross-DC', 'F2: other cross-DC', 'F3: local competitor',
              'B egress queue: sampled peak', 'Whole-network PFC trace events',
              'Gateway control TX / cross-DC data TX']
    units = ['FCT (ms)']*3 + ['MB (100 us queue samples)', 'Event records', '% during active transfer']
    for ax, key, title, unit in zip(axes.flat, keys, titles, units):
        for i, label in enumerate(LABELS):
            sample = [float(r[key]) for r in stats if r['delay_ms']==5 and r['label']==label]
            if key == 'b_egress_sampled_peak_bytes': sample = np.array(sample)/1e6
            ax.bar(i, np.mean(sample), color=COLORS[i], alpha=.6)
            ax.scatter(i+np.linspace(-.12, .12, len(sample)), sample, color=COLORS[i], s=22, zorder=3)
        ax.set(xticks=range(5), xticklabels=SHORT, title=title, ylabel=unit)
        ax.tick_params(axis='x', labelsize=8); ax.grid(axis='y', alpha=.2)
    fig.suptitle('5 ms propagation; bars = mean, dots = all 3 paired seeds (not confidence intervals)')
    save(fig, out, 'fig05-ablation')


def delay_figure(stats, out):
    fig, axes = plt.subplots(2, 2, figsize=(11, 7), layout='constrained')
    keys = ['fct_0_41_ms', 'fct_0_59_ms', 'fct_42_41_ms', 'control_to_A_data_percent']
    titles = ['F1: affected cross-DC', 'F2: other cross-DC', 'F3: local competitor', 'Control traffic overhead']
    for label, color in zip(['dcqcn', 'candidate-history', 'candidate-snapshot'],
                            [COLORS[0], COLORS[3], COLORS[4]]):
        rr = [r for r in stats if r['label']==label]
        delays = sorted(set(r['delay_ms'] for r in rr))
        for ax, key, title in zip(axes.flat, keys, titles):
            means, low, high = [], [], []
            for delay in delays:
                vals = [float(r[key]) for r in rr if r['delay_ms']==delay]
                means.append(np.mean(vals)); low.append(min(vals)); high.append(max(vals))
            ax.plot(delays, means, 'o-', color=color, label=label)
            ax.fill_between(delays, low, high, alpha=.12, color=color)
            ax.set(title=title, xlabel='One-way DCI propagation (ms)', xticks=delays,
                   ylabel='FCT (ms)' if key.startswith('fct') else 'Control / data (%)')
            ax.grid(alpha=.2)
    for ax in axes.flat: ax.legend(fontsize=8)
    fig.suptitle('Mean and min-max across 3 paired seeds; headroom recomputed at each delay\n'
                 '10 ms R3: common supply < 3 RTT; finite-flow FCT only, not convergence evidence', fontsize=11)
    save(fig, out, 'fig06-delay-sensitivity')


def feedback_figure(locate, out):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4), layout='constrained')
    for label, color, style in [('default-history', COLORS[1], '-'),
                                ('default-control', COLORS[1], '--'),
                                ('candidate-history', COLORS[3], '-'),
                                ('candidate-control', COLORS[3], '--')]:
        run = locate(label)
        b = gateway(run, 81, stop=1.2)
        t = metric_array(b[1], 'time_s')*1000
        count = metric_array(b[1], 'decrease_count') + metric_array(b[2], 'decrease_count')
        axes[0].plot(t, count, style, color=color, label=label)
        received = {}
        for r in rows(run / 'metadata.json.flow-state.csv'):
            time = int(r['time_ns'])/1e9
            if time > 1.2: break
            if int(r['flow']) in (1, 2):
                received[time] = received.get(time, 0) + int(r['rx_payload_bytes'])
        axes[1].plot(np.array(list(received))*1000, np.array(list(received.values()))/1e9,
                     style, color=color, label=label)
    axes[0].set(title='B multiplicative-decrease events (F1 + F2)', ylabel='Cumulative count')
    axes[1].set(title='Cross-DC unique received payload (F1 + F2)', ylabel='GB')
    for ax in axes:
        ax.set(xlabel='Time (ms)', xlim=(10, 1200)); ax.grid(alpha=.2); ax.legend(fontsize=8)
    fig.suptitle('Seed 1: solid = local competitor; dashed = no competitor; history mode')
    save(fig, out, 'fig07-feedback-control')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve(); out = root / 'figures'; out.mkdir(exist_ok=True)
    experiments = []
    for phase in ('core', 'delay'):
        path = root / (phase+'-results.json')
        if path.exists():
            result = json.loads(path.read_text())
            plan = json.loads((root/(phase+'-manifest.json')).read_text())['experiments']
            assert len(result)==len(plan), 'experiment phase is incomplete'
            assert all(r['valid'] for r in result), 'integrity gate failed'
            experiments.extend(result)
    stats = []
    for item in experiments:
        stats.append(statistics(item))
        print('audited', item['label'], item['delay_ms'], item['seed'], flush=True)
    write_csv(out / 'experiment-metrics.csv', stats)
    def locate(label):
        return Path(next(r['run_dir'] for r in experiments if r['label']==label and r['seed']==1 and r['delay_ms']==5))
    run = locate('candidate-history')
    topology_figure(run, out); timeline_figure(out)
    reaction_figure(run, locate('candidate-control'), out)
    prediction_figure(run, out); ablation_figure(stats, out)
    if (root/'delay-results.json').exists(): delay_figure(stats, out)
    feedback_figure(locate, out)


if __name__ == '__main__':
    main()
