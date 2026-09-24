#!/usr/bin/env python3
"""Common-window receiver bottleneck metrics and publication-friendly plots."""
import argparse
import csv
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

COLORS = {'dcqcn':'#d55e00', 'hpcc':'#0072b2', 'timely':'#009e73', 'reactive-cnp':'#cc79a7', 'predictive-cnp':'#111111'}

def read(path):
    with path.open() as f:
        return list(csv.DictReader(f))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    figures = root / 'figures'
    figures.mkdir(exist_ok=True)
    metrics = []
    for scenario in ['single', 'join', 'finite']:
        if not (root / scenario).exists():
            continue
        fig, axes = plt.subplots(3, 2, figsize=(12, 10), sharex=True)
        fig.suptitle(f'Receiver-side experiment: {scenario}')
        cdf_fig, cdf_ax = plt.subplots(figsize=(6, 4))
        rtt_fig, rtt_ax = plt.subplots(figsize=(8, 4.5))
        for variant, color in COLORS.items():
            folder = root / scenario / variant
            if not (folder / 'run.json').exists():
                continue
            assert json.loads((folder / 'run.json').read_text())['exit_code'] == 0, folder
            b = read(folder / 'bottleneck.csv')
            a = {key: np.array([float(r[key]) for r in b]) for key in b[0]}
            r = read(folder / 'receiver-goodput.csv')
            rtt_rows = read(folder / 'measured-rtt.csv')
            # Re-bin each recorded interval into the shared 1 ms time grid.
            stop = float(json.loads((folder / 'metadata.json').read_text())['simulator_stop_time_s'])
            bins = int(round(stop / .001))
            count = 1 if scenario == 'single' else 4
            flow_rate = np.zeros((count, bins))
            for row in r:
                start_s = int(row['interval_start_ns']) / 1e9
                end_s = int(row['time_ns']) / 1e9
                rate_gbps = float(row['goodput_bps']) / 1e9
                src = int(row['src'])
                if src >= count or end_s <= start_s:
                    continue
                first_bin = max(0, int(start_s / .001))
                last_bin = min(bins - 1, int(np.ceil(end_s / .001)) - 1)
                for idx in range(first_bin, last_bin + 1):
                    overlap_s = max(0.0, min(end_s, (idx + 1) * .001) - max(start_s, idx * .001))
                    flow_rate[src, idx] += rate_gbps * overlap_s / .001
            times = (np.arange(bins)+1)*.001
            agg = flow_rate.sum(axis=0)
            start = .05 if scenario == 'join' else .01
            end = min(.17, stop - .001)
            win = (a['time_s'] > start+1e-9) & (a['time_s'] <= end+1e-9)
            rxwin = (times > start+1e-9) & (times <= end+1e-9)
            steady = (times > end-.03+1e-9) & (times <= end+1e-9)
            per_flow = flow_rate[:, steady].mean(axis=1)
            jain = per_flow.sum()**2/(count*(per_flow**2).sum()) if (per_flow**2).sum() else 0
            fct = read(folder / 'fct.csv')
            fcts = np.array([int(x['fct_ns'])/1e6 for x in fct])
            meta = json.loads((folder / 'metadata.json').read_text())
            path_metrics = meta['flow_path_metrics']
            hold = max(.03, 3*max(int(flow['base_rtt_ns']) for flow in path_metrics)/1e9)
            required = int(np.ceil(hold/.001))
            settle = None
            # Payload fair share accounts for the configured transport header.
            payload_size = int(meta['packet_payload_size'])
            data_header_bytes = int(meta['data_header_bytes'])
            path_capacity = min(float(meta['dci_rate_bps']),
                                min(int(flow['bottleneck_rate_bps']) for flow in path_metrics))
            target = path_capacity / count / 1e9 * payload_size / (payload_size + data_header_bytes)
            if scenario == 'join':
                ok = np.all((flow_rate >= target*.9) & (flow_rate <= target*1.1), axis=0)
                for i in range(bins-required):
                    if times[i] > start and times[i+required-1] <= end and np.all(ok[i:i+required]):
                        settle = round((times[i]-.001-start)*1000, 3)
                        break
            m = dict(scenario=scenario, variant=variant, window_start_s=start, window_end_s=end,
                     queue_peak_MB=float(a['queue_bytes'][win].max()/1e6),
                     queue_p95_MB=float(np.percentile(a['queue_bytes'][win],95)/1e6),
                     queue_area_MB_ms=float(a['queue_bytes'][win].sum()/1e6),
                     goodput_Gbps=float(agg[rxwin].mean()),
                     steady_goodput_Gbps=float(agg[steady].mean()), jain_steady=float(jain),
                     settling_ms=settle, fair_payload_target_Gbps=target, completed=len(fcts), total_flows=count,
                     fct_median_ms=float(np.median(fcts)) if len(fcts) else None,
                     fct_max_ms=float(fcts.max()) if len(fcts) else None,
                     measured_rtt_p50_ns=float(np.median([int(x['rtt_p50_ns']) for x in rtt_rows])) if rtt_rows else None,
                     measured_rtt_p95_ns=float(np.median([int(x['rtt_p95_ns']) for x in rtt_rows])) if rtt_rows else None,
                     admission_drops=int(a['admission_drop_packets'][-1]),
                     ecn_packets=int(a['ecn_packets'][-1]), cnp_sent=int(a['cnp_sent'][-1]),
                     pfc_pause_sent=sum(x['event_type']=='2' for x in read(folder/'pfc.csv')),
                     steady_per_flow_Gbps=per_flow.tolist())
            if scenario == 'finite':
                m['jain_steady'] = None  # finished flows are not a fairness sample
                m['steady_goodput_Gbps'] = None
            metrics.append(m)
            series=[a['queue_bytes']/1e6, a['arrival_bps']/1e9, a['sender_wire_bps']/1e9,
                    a['departure_bps']/1e9, a['virtual_queue_bytes']/1e6, a['admission_drop_packets']]
            labels=['Receiver queue (MB)', 'Receiver arrival (Gbps)', 'Sender wire TX sum (Gbps)',
                    'Receiver egress (Gbps)', 'Source virtual queue (MB)', 'Admission drops (cumulative)']
            for ax, values, label in zip(axes.flat,series,labels):
                ax.plot(a['time_s']*1000, values, color=color, label=variant, lw=1.4)
                ax.set_ylabel(label)
                ax.grid(alpha=.25)
                ax.axvline(start*1000, color='gray', linestyle=':', lw=1)
            if len(fcts):
                cdf_ax.step(np.sort(fcts), np.arange(1,len(fcts)+1)/count, where='post', label=variant, color=color)
            if rtt_rows:
                rtt_by_time = {}
                for sample in rtt_rows:
                    t_ms = int(sample['time_ns']) / 1e6
                    rtt_by_time.setdefault(t_ms, []).append((int(sample['rtt_p50_ns']), int(sample['rtt_p95_ns'])))
                points = sorted((t, np.median([x[0] for x in values]) / 1e6,
                                 np.median([x[1] for x in values]) / 1e6)
                                for t, values in rtt_by_time.items())
                rtt_ax.plot([x[0] for x in points], [x[1] for x in points], color=color, label=f'{variant} p50')
                rtt_ax.plot([x[0] for x in points], [x[2] for x in points], color=color, linestyle='--', label=f'{variant} p95')
            if variant in ['reactive-cnp', 'predictive-cnp']:
                trace, ax = plt.subplots(2,1,figsize=(9,6),sharex=True)
                ax[0].plot(a['time_s']*1000,a['queue_bytes']/1e6,label='actual queue')
                ax[0].plot(a['time_s']*1000,a['predicted_queue_bytes']/1e6,label='H-step prediction')
                ax[0].set_ylabel('MB'); ax[0].legend()
                for key,label in [('target_bps','receiver target'),('source_target_bps','applied source target'),('telemetry_bps','delayed source TX')]:
                    ax[1].plot(a['time_s']*1000,a[key]/1e9,label=label)
                ax[1].set_ylabel('Gbps'); ax[1].set_xlabel('Time (ms)'); ax[1].legend()
                trace.suptitle(f'{scenario}: {variant} control trace')
                trace.tight_layout(); trace.savefig(figures/f'{scenario}-{variant}-control.png',dpi=170); plt.close(trace)
        for ax in axes[-1]: ax.set_xlabel('Time (ms)')
        axes[0,0].legend(fontsize=8)
        fig.tight_layout(); fig.savefig(figures/f'{scenario}-overview.png',dpi=170); plt.close(fig)
        cdf_ax.set(xlabel='FCT (ms)',ylabel='Completed / all flows',ylim=(0,1.05))
        handles,_=cdf_ax.get_legend_handles_labels()
        if handles: cdf_ax.legend()
        cdf_ax.grid(alpha=.25); cdf_fig.tight_layout(); cdf_fig.savefig(figures/f'{scenario}-fct.png',dpi=170); plt.close(cdf_fig)
        rtt_ax.set(xlabel='Time (ms)', ylabel='Measured RTT (ms)',
                   title=f'{scenario}: sender-to-ACK RTT')
        rtt_ax.grid(alpha=.25)
        if rtt_ax.get_legend_handles_labels()[0]: rtt_ax.legend(ncol=2, fontsize=8)
        rtt_fig.tight_layout(); rtt_fig.savefig(figures/f'{scenario}-rtt.png',dpi=170); plt.close(rtt_fig)
    (root/'metrics.json').write_text(json.dumps(metrics,indent=2,ensure_ascii=False)+'\n')
    if metrics:
        with (root/'metrics.csv').open('w') as f:
            writer=csv.DictWriter(f,fieldnames=list(metrics[0]));writer.writeheader();writer.writerows(metrics)
    for m in metrics:
        print(json.dumps(m,ensure_ascii=False))

if __name__ == '__main__':
    main()
