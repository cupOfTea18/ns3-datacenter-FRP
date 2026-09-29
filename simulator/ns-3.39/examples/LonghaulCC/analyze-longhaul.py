#!/usr/bin/env python3
"""Path-constrained references, explicit windows, and finite-flow validation."""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path
from statistics import mean, median


def read_csv(path):
    if not path.is_file():
        return []
    with path.open() as stream:
        return list(csv.DictReader(stream))


def write_csv(path, rows):
    if not rows:
        path.write_text("")
        return
    fields = list(dict.fromkeys(k for r in rows for k in r))
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def quantile(values, q):
    if not values:
        return math.nan
    values = sorted(values)
    pos = q * (len(values) - 1)
    lo, hi = math.floor(pos), math.ceil(pos)
    return values[lo] + (values[hi] - values[lo]) * (pos - lo)


def key(row):
    return tuple(int(row[k]) for k in ("src", "dst", "sport", "dport", "pg"))


def path_reference(flows, payload, header):
    """Equal-weight max-min PAYLOAD bps under directed DATA+ACK wire constraints.

    Full payload packets and one ACK per max(payload, configured ACK interval)
    bytes; excludes retransmissions, PFC/CNP/STATE overhead and window limits.
    This is an analysis reference, not a protocol fairness guarantee.
    """
    coefficients, capacity = defaultdict(dict), {}
    for f in flows:
        if not f.get("data_path") or not f.get("ack_path"):
            return None
        for name, weight in (("data_path", (payload + header) / payload),
                             ("ack_path", f["ack_wire_bytes"] / max(payload, f["ack_interval_bytes"]))):
            for u, v, cap in f[name]:
                edge = u, v
                if edge in capacity and capacity[edge] != cap:
                    raise ValueError("inconsistent directed-link capacity")
                capacity[edge] = cap
                coefficients[edge][f["id"]] = coefficients[edge].get(f["id"], 0) + weight
    rates = dict.fromkeys((f["id"] for f in flows), 0.0)
    pending = set(rates)
    while pending:
        steps = {}
        for edge, users in coefficients.items():
            slope = sum(w for fid, w in users.items() if fid in pending)
            if slope:
                used = sum(w * rates[fid] for fid, w in users.items())
                steps[edge] = max(0.0, (capacity[edge] - used) / slope)
        delta = min(steps.values())
        for fid in pending:
            rates[fid] += delta
        saturated = [e for e, step in steps.items() if abs(step - delta) <= max(1e-8, delta * 1e-10)]
        pending -= {fid for e in saturated for fid in coefficients[e]}
    return rates


def settling(samples, start, end, target, hold, step, tolerance=.10):
    samples = [(t, v) for t, v in samples if start <= t <= end]
    if end - start < hold:
        return "insufficient_window", None
    if not samples or samples[-1][0] - samples[0][0] < hold:
        return "insufficient_samples", None
    # A candidate requires uninterrupted sampling, not just two distant points.
    run_start = None
    previous = None
    had_coverage = False
    coverage_start = None
    for t, v in samples:
        gap = previous is not None and t - previous > step * 1.5
        if coverage_start is None or gap:
            coverage_start = t
        had_coverage |= t - coverage_start >= hold - 1e-9
        good = target * (1-tolerance) <= v <= target * (1+tolerance)
        if not good:
            run_start = None
        elif run_start is None or gap:
            run_start = t
        if run_start is not None and t - run_start >= hold - 1e-9:
            return "converged", (run_start-start)*1000
        previous = t
    return ("not_converged" if had_coverage else "insufficient_samples"), None


def convergence_summary(rows, kind):
    counts = Counter(r[f"{kind}_status"] for r in rows)
    evaluated = counts["converged"] + counts["not_converged"]
    converged = [r[f"{kind}_settling_10_ms"] for r in rows if r[f"{kind}_status"] == "converged"]
    return {f"{kind}_evaluated_stages": evaluated,
            f"{kind}_not_converged_stages": counts["not_converged"],
            f"{kind}_not_converged_fraction": counts["not_converged"]/evaluated if evaluated else math.nan,
            f"{kind}_insufficient_window_stages": counts["insufficient_window"],
            f"{kind}_insufficient_sample_stages": counts["insufficient_samples"],
            f"{kind}_unavailable_reference_stages": counts["unavailable_reference"],
            f"{kind}_converged_subset_median_ms": median(converged) if converged else math.nan}


def window_metrics(rows, start, end, capacity):
    selected = [r for r in rows if start <= int(r["time_ns"])*1e-9 <= end]
    rates = [float(r["tx_bps"])/capacity for r in selected]
    queues = [int(r["queue_bytes"]) for r in selected]
    return {"samples": len(selected), "utilization_mean": mean(rates) if rates else math.nan,
            "utilization_p50": quantile(rates,.5), "utilization_p95": quantile(rates,.95),
            "queue_sample_max_bytes": max(queues, default=0),
            "queue_sample_p95_bytes": quantile(queues,.95)}


def relation_check(contract, flows, states, stop, hold):
    """Use actual unique-supply progress and windowed TX; ACK completion alone is insufficient."""
    relation = contract.get("relation", "unspecified")
    def supplying(fid, t):
        f = flows[fid-1]
        return f["start_time_s"] <= t < f["supply_end"] and t <= stop
    def sends(fid, start, end):
        samples = states.get(fid, [])
        values = [(int(r["time_ns"])*1e-9, int(r["tx_payload_bytes"])) for r in samples]
        before = [v for t,v in values if t <= start]
        after = [v for t,v in values if t <= end]
        return bool(after and after[-1] > (before[-1] if before else 0))
    result = {"relation": relation, "status": "not_applicable"}
    if relation in ("join", "mechanism"):
        event = contract["event_s"]
        required = contract["persistent_flows"] + contract.get("joining_flows", [])
        if relation == "mechanism":
            required += contract["competitor_flows"]
        end = min([flows[i-1]["supply_end"] for i in required]+[stop])
        result.update(event_s=event, common_supply_end_s=end,
                      status="pass" if end-event >= hold and all(supplying(i,event) and sends(i,event,end) for i in required) else "fail")
        if relation == "mechanism":
            a=flows[contract["affected_flow"]-1]; b=flows[contract["unaffected_flow"]-1]
            edges=lambda f:{tuple(e[:2]) for e in f["data_path"]}
            target=tuple(contract["target_link"])
            shared={e for e in edges(a)&edges(b) if e[0]==contract["b_gateway"]}
            valid=bool(shared) and target in edges(a) and target not in edges(b)
            valid &= all(target in edges(flows[i-1]) for i in contract["competitor_flows"])
            result["path_contract_valid"]=valid
            if not valid: result["status"]="fail"
    elif relation == "exit":
        ends=[flows[i-1]["supply_end"] for i in contract["departing_flows"]]
        event=max(ends)
        # Exit is last actual payload TX, which may be later than first-pass supply on loss.
        last_tx=[]
        for fid in contract["departing_flows"]:
            previous=0; last=0
            for r in states.get(fid,[]):
                if int(r["tx_payload_bytes"])>previous: last=int(r["time_ns"])*1e-9
                previous=int(r["tx_payload_bytes"])
            last_tx.append(last)
        event=max([event]+last_tx+[flows[i-1]["completion_ns"]*1e-9 for i in contract["departing_flows"]])
        end=min([flows[i-1]["supply_end"] for i in contract["persistent_flows"]]+[stop])
        result.update(event_s=event, remaining_supply_end_s=end,
                      status="pass" if all(flows[i-1]["completion_ns"] for i in contract["departing_flows"]) and event<stop and end-event>=hold and all(sends(i,event,event+hold) for i in contract["persistent_flows"]) else "fail")
    elif relation == "coexist":
        checks=[]
        for fid in contract["probe_flows"]:
            event=flows[fid-1]["start_time_s"]
            active=[i for i in contract["background_flows"] if supplying(i,event) and sends(i,event,min(event+hold,stop))]
            checks.append({"probe":fid,"event_s":event,"background_with_supply_and_tx":active})
        result.update(probes=checks,status="pass" if all(len(c["background_with_supply_and_tx"])==len(contract["background_flows"]) for c in checks) else "fail")
    return result


def r3_evidence(run_dir, identity):
    grouped=defaultdict(dict)
    for path in run_dir.glob("r3.gateway-*.csv"):
        if ".events." in path.name or ".packets." in path.name: continue
        with path.open() as stream:
            for r in csv.DictReader(stream):
                k=(r["role"],int(r["group"])); t=float(r["time_s"])
                if t not in grouped[k]: grouped[k][t]={**r,"group_queue":0,"group_tx":0}
                grouped[k][t]["group_queue"]+=int(r["queue_bytes"])
                grouped[k][t]["group_tx"]+=int(r["tx_bytes"])
    result=[]
    for (role,gid), samples in grouped.items():
        if role!="A": continue
        b=grouped.get(("B",gid),{}); times=sorted(b)
        ordered=sorted(samples)
        for index,t in enumerate(ordered[:-1]):
            r=samples[t]; end=float(r.get("prediction_end_s",t)); pos=bisect.bisect_left(times,end)
            observed=b[times[pos]]["group_queue"] if pos<len(times) else None
            next_r=samples[ordered[index+1]]
            result.append({**identity,"group":gid,"time_s":t,"prediction_end_s":end,
                "B_sample_time_s":times[pos] if pos<len(times) else None,
                "B_future_queue_bytes":observed,"prediction_error_bytes":float(r["predicted_queue_bytes"])-observed if observed is not None and r.get("snapshot_valid")=="1" else None,
                "A_group_queue_bytes":r["group_queue"],"predicted_queue_bytes":r["predicted_queue_bytes"],
                "queue_used_bytes":r.get("queue_used_bytes"),"snapshot_seq":r["snapshot_seq"],
                "snapshot_queue_bytes":r.get("snapshot_queue_bytes"),"group_target_bps":r.get("group_target_bps"),
                "pre_port_target_bps":r.get("pre_port_target_bps"),"paused":r.get("paused"),
                "snapshot_valid":r.get("snapshot_valid"),
                "next_interval_actual_bps":(next_r["group_tx"]-r["group_tx"])*8/(ordered[index+1]-t)})
    return result


def analyze_run(run_dir):
    runner=json.loads((run_dir/"runner-metadata.json").read_text())
    meta=json.loads((run_dir/"metadata.json").read_text())
    stop=meta["simulator_stop_time_s"]
    identity={"algorithm":meta["algorithm"],"queue_mode":runner.get("r3_queue_mode"),
              "scenario":runner.get("scenario",meta["scenario"]),"seed":meta["rng_seed"],"run":meta["rng_run"],"run_dir":str(run_dir)}
    summary_path=run_dir/"metadata.json.summary.json"
    if not summary_path.is_file():
        raise ValueError(f"{run_dir}: common final counters missing; old data cannot certify supply/completion")
    final=json.loads(summary_path.read_text()); final_by_id={r["flow"]:r for r in final["flows"]}
    flows=[{**r,"id":i+1} for i,r in enumerate(meta["flow_path_metrics"])]
    fcts=read_csv(run_dir/"fct.csv"); fct_counts=Counter(key(r) for r in fcts)
    flow_rows=[]
    for f in flows:
        edges={tuple(e[:2]) for e in f.get("data_path",[])}
        f["direction"]="left-to-right" if (meta["dci_left"],meta["dci_right"]) in edges else "right-to-left" if (meta["dci_right"],meta["dci_left"]) in edges else "local"
        r=final_by_id[f["id"]]; f["completion_ns"]=r["completion_ns"]; f["supply_end"]=r["supply_end_ns"]*1e-9 if r["unique_sent_bytes"]>=f["size_bytes"] else stop
        done=r["completion_ns"]>0
        flow_rows.append({**identity,"flow":f["id"],"direction":f["direction"],**{k:f[k] for k in ("src","dst","sport","dport","pg","size_bytes")},
                         "status":"completed" if done else "incomplete", "fct_records":fct_counts[key(f)],
                         "rx_payload_bytes":r["rx_payload_bytes"],"tx_payload_bytes":r["tx_payload_bytes"],
                         "unique_sent_bytes":r["unique_sent_bytes"],"supply_end_ns":r["supply_end_ns"],
                         "retransmitted_payload_bytes":r["tx_payload_bytes"]-r["unique_sent_bytes"],
                         "remaining_rx_bytes":max(0,f["size_bytes"]-r["rx_payload_bytes"]),
                         "completion_ns":r["completion_ns"]})
    samples={}; states=defaultdict(list)
    for r in read_csv(run_dir/"metadata.json.flow-state.csv"):states[int(r["flow"])].append(r)
    for kind,filename,field in (("sender","sender-rate.csv","tx_payload_bps"),("receiver","receiver-goodput.csv","goodput_bps")):
        by_key=defaultdict(list)
        for r in read_csv(run_dir/filename):by_key[key(r)].append((int(r["time_ns"])*1e-9,float(r[field])))
        samples[kind]=by_key
    events=sorted({stop}|{f["start_time_s"] for f in flows if f["start_time_s"]<stop}|{f["supply_end"] for f in flows if f["supply_end"]<stop})
    stages=[]
    for stage,(start,end) in enumerate(zip(events,events[1:])):
        active=[f for f in flows if f["start_time_s"]<=start+1e-12<f["supply_end"]]
        reference=path_reference(active,meta["packet_payload_size"],meta["data_header_bytes"])
        for f in active:
            row={**identity,"stage":stage,"flow":f["id"],"direction":f["direction"],"start_s":start,"end_s":end,"active_flows":len(active),
                 "target_payload_bps":reference[f["id"]] if reference else None,
                 "reference":"directed-path max-min payload with data/ACK overhead; excludes control/retransmissions/windows"}
            hold=max(.020,3*f["base_rtt_ns"]*1e-9)
            for kind in samples:
                data=samples[kind].get(key(f),[])
                status,value=settling(data,start,end,reference[f["id"]],hold,meta[f"{'rate' if kind=='sender' else 'goodput'}_sample_interval_us"]*1e-6) if reference else ("unavailable_reference",None)
                row[f"{kind}_status"]=status;row[f"{kind}_settling_10_ms"]=value
                values=[v for t,v in data if start<=t<=end]
                row[f"{kind}_stage_mean_bps"]=mean(values) if values else math.nan
            stages.append(row)
    contract=runner.get("scenario_contract",{})
    hold=max(.020,3*max(f["base_rtt_ns"] for f in flows)*1e-9)
    relation=relation_check(contract,flows,states,stop,hold)
    windows=[("whole_run",0,stop)]+[(name,start,min(end,stop)) for name,start,end in contract.get("windows",[]) if start<stop]
    if relation.get("relation")=="exit" and relation.get("status")=="pass":
        windows.append(("actual_exit_plus_hold",relation["event_s"],relation["event_s"]+hold))
    directional=[]; dci=read_csv(run_dir/"dci-link.csv")
    for direction in ("left-to-right","right-to-left"):
        rows=[r for r in dci if r["direction"]==direction]
        for name,start,end in windows:
            directional.append({**identity,"direction":direction,"window":name,"start_s":start,"end_s":end,
                                **window_metrics(rows,start,end,meta["dci_rate_bps"])})
    port_rows=defaultdict(list)
    for r in read_csv(run_dir/"metadata.json.queues.csv"):
        port_rows[tuple(int(r[k]) for k in ("node","port","peer","pg"))].append(r)
    queue_metrics=[]
    for (node,port,peer,pg), rows in port_rows.items():
        for name,start,end in windows:
            selected=[r for r in rows if start<=int(r["time_ns"])*1e-9<=end]
            values=[int(r["queue_bytes"]) for r in selected]
            queue_metrics.append({**identity,"node":node,"port":port,"peer":peer,"pg":pg,"window":name,
                                  "sample_max_bytes":max(values,default=0),"sample_p95_bytes":quantile(values,.95),
                                  "paused_sample_fraction":mean(int(r["paused"]) for r in selected) if selected else math.nan})
    write_csv(run_dir/"queue-window-summary.csv",queue_metrics)
    # FCTs remain visibly a completed subset; incomplete/duplicate counts are never dropped.
    fct_values=[int(r["fct_ns"]) for r in fcts]
    run={**identity,"purpose":runner.get("purpose","unknown"),"expected_flows":len(flows),
         "completed_flows":sum(r["status"]=="completed" for r in flow_rows),
         "incomplete_flows":sum(r["status"]!="completed" for r in flow_rows),
         "duplicate_fct_records":sum(max(0,c-1) for c in fct_counts.values()),
         "unknown_fct_records":sum(c for k,c in fct_counts.items() if k not in {key(f) for f in flows}),
         "completed_payload_mismatch_flows":sum(r["status"]=="completed" and r["rx_payload_bytes"]!=r["size_bytes"] for r in flow_rows),
         "remaining_rx_bytes":sum(r["remaining_rx_bytes"] for r in flow_rows),
         "resource_gate": "pass" if final["admission_drop_packets"]==0 else "admission_drops",
         "completed_subset_fct_p50_ns":quantile(fct_values,.5),"completed_subset_fct_p95_ns":quantile(fct_values,.95),
         "admission_drop_packets":final["admission_drop_packets"],"remaining_switch_queue_bytes":final["remaining_switch_queue_bytes"],
         "scenario_relation_status":relation["status"],**convergence_summary(stages,"sender"),**convergence_summary(stages,"receiver")}
    return run,stages,flow_rows,directional,{**identity,**relation},r3_evidence(run_dir,identity)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,required=True)
    parser.add_argument("--output",type=Path)
    args=parser.parse_args(); output=args.output or args.root/"summary.csv"
    output.parent.mkdir(parents=True,exist_ok=True)
    tables=[[] for _ in range(6)]
    for p in sorted(args.root.glob("**/runner-metadata.json")):
        runner=json.loads(p.read_text())
        if runner.get("status")!="ok":continue
        result=analyze_run(p.parent)
        for i,r in enumerate(result): tables[i].extend(r if isinstance(r,list) else [r])
    if not tables[0]:raise SystemExit("no stable successful runs found")
    for name,rows in (("run-summary.csv",tables[0]),(output.name,tables[1]),("flow-summary.csv",tables[2]),
                      ("direction-summary.csv",tables[3]),("r3-evidence.csv",tables[5])):
        write_csv(output.parent/name,rows)
    (output.parent/"scenario-validation.json").write_text(json.dumps(tables[4],indent=2)+"\n")
    print(f"analyzed {len(tables[0])} runs; outputs: {output.parent}")
    return 0


if __name__=="__main__":
    raise SystemExit(main())
