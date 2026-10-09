#!/usr/bin/env python3
"""Plot explicit directions and convergence outcomes from the current analysis schema."""
import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path


def read(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def main():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,required=True)
    parser.add_argument("--summary",type=Path)
    parser.add_argument("--output-dir",type=Path)
    parser.add_argument("--scenario")
    parser.add_argument("--all-scenarios",action="store_true")
    args=parser.parse_args(); out=args.output_dir or args.root/"plots";out.mkdir(parents=True,exist_ok=True)
    stages=read(args.summary or args.root/"summary.csv")
    runs=read((args.summary or args.root/"summary.csv").parent/"run-summary.csv")
    scenarios=sorted({r["scenario"] for r in runs})
    if args.scenario:scenarios=[args.scenario.upper()]
    for scenario in scenarios:
        selected=[r for r in runs if r["scenario"].upper()==scenario.upper()]
        fig,axes=plt.subplots(2,2,figsize=(12,7),sharex=True)
        fig_status,ax_status=plt.subplots(figsize=(10,5))
        fig_rates, rate_axes=plt.subplots(2,2,figsize=(12,7),sharex=True)
        fig_rtt, ax_rtt=plt.subplots(figsize=(10,4))
        labels=[]
        for i,r in enumerate(selected):
            path=Path(r["run_dir"]);meta=json.loads((path/"metadata.json").read_text())
            label=f'{r["algorithm"]}/{r["queue_mode"] or "native"}/seed{r["seed"]}-run{r["run"]}'
            labels.append(label)
            directions={}
            for f in meta["flow_path_metrics"]:
                edges={tuple(e[:2]) for e in f.get("data_path",[])}
                direct="left-to-right" if (meta["dci_left"],meta["dci_right"]) in edges else "right-to-left" if (meta["dci_right"],meta["dci_left"]) in edges else "local"
                directions[tuple(str(f[k]) for k in ("src","dst","sport","dport","pg"))]=direct
            for row_index,filename,field in ((0,"sender-rate.csv","tx_payload_bps"),(1,"receiver-goodput.csv","goodput_bps")):
                values=defaultdict(float)
                step=meta["rate_sample_interval_us" if row_index==0 else "goodput_sample_interval_us"]*1000
                # Rebin byte deltas, including short completion intervals. Summing
                # only equal endpoint timestamps would omit other active flows.
                byte_field="tx_payload_bytes" if row_index==0 else "goodput_bytes"
                for point in read(path/filename):
                    direct=directions[tuple(point[k] for k in ("src","dst","sport","dport","pg"))]
                    start,end=int(point["interval_start_ns"]),int(point["time_ns"])
                    if end<=start:continue
                    for index in range(start//step,(end-1)//step+1):
                        overlap=max(0,min(end,(index+1)*step)-max(start,index*step))
                        values[(direct,(index+1)*step)]+=int(point[byte_field])*8*1e9*overlap/(end-start)/step
                for col,direct in enumerate(("left-to-right","right-to-left")):
                    points=sorted((t,v) for (d,t),v in values.items() if d==direct)
                    rate_axes[row_index,col].plot([t*1e-6 for t,v in points],[v/1e9 for t,v in points],label=label)
                    rate_axes[row_index,col].set_title(direct+" "+("sender payload TX" if row_index==0 else "receiver goodput"))
                    rate_axes[row_index,col].set_ylabel("Gbps")
            rtts=defaultdict(list)
            for point in read(path/"measured-rtt.csv"):rtts[int(point["time_ns"])].append(float(point["rtt_p50_ns"]))
            points=sorted((t,sum(v)/len(v)) for t,v in rtts.items())
            ax_rtt.plot([t*1e-6 for t,v in points],[v*1e-6 for t,v in points],label=label)
            data=read(path/"dci-link.csv")
            for col,direction in enumerate(("left-to-right","right-to-left")):
                points=[p for p in data if p["direction"]==direction]
                x=[int(p["time_ns"])*1e-6 for p in points]
                axes[0,col].plot(x,[float(p["tx_bps"])/meta["dci_rate_bps"] for p in points],label=label)
                axes[1,col].plot(x,[int(p["queue_bytes"])/1e6 for p in points],label=label)
                axes[0,col].set_title(direction);axes[1,col].set_xlabel("simulation time (ms)")
            relevant=[s for s in stages if s["run_dir"]==str(path)]
            counts=Counter(s["receiver_status"] for s in relevant)
            bottom=0
            for status in ("converged","not_converged","insufficient_window","insufficient_samples","unavailable_reference"):
                ax_status.bar(i,counts[status],bottom=bottom,label=status if i==0 else None)
                bottom+=counts[status]
            ax_status.text(i,bottom+.1,f'{r["completed_flows"]}/{r["expected_flows"]} completed',ha="center",fontsize=8)
        rate_axes[0,0].legend(fontsize=7)
        for ax in rate_axes.flat:ax.grid(alpha=.25);ax.set_xlabel("simulation time (ms)")
        fig_rates.suptitle(scenario+": payload rates by direction")
        fig_rates.tight_layout();fig_rates.savefig(out/(scenario.lower()+"-rates.png"),dpi=140);plt.close(fig_rates)
        ax_rtt.set_title(scenario+": mean of per-flow interval RTT medians (not a packet percentile)")
        ax_rtt.set_xlabel("simulation time (ms)");ax_rtt.set_ylabel("RTT (ms)");ax_rtt.legend(fontsize=7)
        fig_rtt.tight_layout();fig_rtt.savefig(out/(scenario.lower()+"-rtt.png"),dpi=140);plt.close(fig_rtt)
        for ax in axes.flat:ax.grid(alpha=.25)
        axes[0,0].set_ylabel("WAN wire utilization");axes[1,0].set_ylabel("WAN egress sampled queue (MB)")
        axes[0,0].legend(fontsize=7);fig.suptitle(scenario+": complete time axis; no mixed-direction/tail-window aggregate")
        fig.tight_layout();fig.savefig(out/(scenario.lower()+"-dci.png"),dpi=140);plt.close(fig)
        ax_status.set_xticks(range(len(labels)),labels,rotation=25,ha="right")
        ax_status.set_ylabel("receiver flow-stages");ax_status.set_title(scenario+": reference convergence (±10%, hold=max(3 RTT,20 ms))")
        ax_status.legend(fontsize=8);fig_status.tight_layout();fig_status.savefig(out/(scenario.lower()+"-status.png"),dpi=140);plt.close(fig_status)
    for run in runs:
        if run.get("proposed_version")!="4": continue
        path=Path(run["run_dir"])
        fig,axes=plt.subplots(2,1,figsize=(11,7),sharex=True)
        for source in sorted(path.glob("r4.gateway-*.flows.csv")):
            grouped=defaultdict(list)
            for row in read(source): grouped[(row["node"],row["role"],row["flow"],row["generation"])].append(row)
            for key,rows in grouped.items():
                label="/".join(key)
                t=[int(r["time_ns"])*1e-6 for r in rows]
                axes[0].plot(t,[float(r["queue_bytes"])/1e3 for r in rows],label=label+" queue")
                if key[1]=="A":
                    axes[0].plot(t,[float(r["predicted_queue_bytes"])/1e3 for r in rows],ls=":",label=label+" predicted B")
                axes[1].plot(t,[float(r["target_bps"])/1e9 for r in rows],label=label+" target")
                axes[1].plot(t,[float(r["service_bps"])/1e9 for r in rows],ls=":",label=label+" service estimate")
        axes[0].set_ylabel("wire queue (kB)"); axes[1].set_ylabel("Gbps"); axes[1].set_xlabel("simulation time (ms)")
        for ax in axes: ax.legend(fontsize=6); ax.grid(alpha=.2)
        fig.suptitle("R4 per-flow control: targets and estimates, not actual throughput")
        fig.tight_layout()
        # Save per run to avoid collisions across service/queue ablations.
        fig.savefig(path/"r4-control.png",dpi=140); plt.close(fig)
    print(f"wrote plots to {out}")


if __name__=="__main__":main()
