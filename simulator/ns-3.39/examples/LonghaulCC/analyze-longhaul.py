#!/usr/bin/env python3
"""Analyze longhaul CSVs with one common settling-time definition."""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from statistics import mean, median, pstdev


NS3_ROOT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class FlowSpec:
    src: int
    dst: int
    sport: int
    pg: int
    dport: int
    size_bytes: int
    start_s: float
    direction: str
    base_rtt_ns: int
    path_hops: int
    bottleneck_rate_bps: int
    bdp_bytes: int
    window_bytes: int

    @property
    def key(self) -> tuple[int, int, int, int, int]:
        return self.src, self.dst, self.sport, self.dport, self.pg


def quantile(values: list[float], fraction: float) -> float:
    if not values:
        return float("nan")
    values = sorted(values)
    index = fraction * (len(values) - 1)
    lower, upper = math.floor(index), math.ceil(index)
    if lower == upper:
        return values[lower]
    return values[lower] + (values[upper] - values[lower]) * (index - lower)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file() or path.stat().st_size == 0:
        return []
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def resolve_input(raw_path: str, run_dir: Path) -> Path:
    path = Path(raw_path)
    if path.is_absolute() and path.is_file():
        return path
    candidates = [Path.cwd() / path, NS3_ROOT / path, run_dir / path,
                  Path(__file__).resolve().parent / path]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise SystemExit(f"input file not found: {raw_path}")


def topology_sides(path: Path, dci_left: int, dci_right: int) -> tuple[set[int], set[int]]:
    tokens = path.read_text().split()
    if len(tokens) < 4:
        raise SystemExit(f"malformed topology header: {path}")
    node_count, switch_count, _, link_count = map(int, tokens[:4])
    link_offset = 4 + switch_count
    graph: list[list[int]] = [[] for _ in range(node_count)]
    for index in range(link_count):
        offset = link_offset + index * 5
        if offset + 1 >= len(tokens):
            raise SystemExit(f"malformed topology link {index}: {path}")
        src, dst = int(tokens[offset]), int(tokens[offset + 1])
        if src >= node_count or dst >= node_count:
            raise SystemExit(f"topology link {index} has an invalid node: {path}")
        if {src, dst} == {dci_left, dci_right}:
            continue
        graph[src].append(dst)
        graph[dst].append(src)

    def reachable(start: int) -> set[int]:
        seen = {start}
        pending = [start]
        while pending:
            current = pending.pop()
            for neighbor in graph[current]:
                if neighbor not in seen:
                    seen.add(neighbor)
                    pending.append(neighbor)
        return seen

    return reachable(dci_left), reachable(dci_right)


def read_flow_specs(path: Path, topology_path: Path,
                    dci_left: int, dci_right: int,
                    path_metrics: list[dict[str, object]]) -> list[FlowSpec]:
    tokens = path.read_text().split()
    if not tokens:
        raise SystemExit(f"empty flow file: {path}")
    count = int(tokens[0])
    if len(tokens) != 1 + count * 6:
        raise SystemExit(f"flow count does not match file contents: {path}")
    left_side, right_side = topology_sides(topology_path, dci_left, dci_right)
    metrics_by_key = {
        (int(item["src"]), int(item["dst"]), int(item["sport"]),
         int(item["dport"]), int(item["pg"])): item
        for item in path_metrics
    }
    next_port: dict[tuple[int, int], int] = {}
    flows = []
    for index in range(count):
        offset = 1 + index * 6
        src, dst, pg, dport = map(int, tokens[offset:offset + 4])
        size_bytes = int(tokens[offset + 4])
        start_s = float(tokens[offset + 5])
        pair = src, dst
        sport = next_port.get(pair, 10000)
        next_port[pair] = sport + 1
        if src in left_side and dst in right_side:
            direction = "left-to-right"
        elif src in right_side and dst in left_side:
            direction = "right-to-left"
        else:
            direction = "local"
        key = src, dst, sport, dport, pg
        metric = metrics_by_key.get(key)
        if metric is None:
            raise SystemExit(f"flow path metrics are missing for {key}: {path}")
        flows.append(FlowSpec(src, dst, sport, pg, dport, size_bytes, start_s, direction,
                              int(metric["base_rtt_ns"]), int(metric["path_hops"]),
                              int(metric["bottleneck_rate_bps"]),
                              int(metric["bdp_bytes"]), int(metric["window_bytes"])))
    return flows


def flow_key(row: dict[str, str]) -> tuple[int, int, int, int, int]:
    return (int(row["src"]), int(row["dst"]), int(row["sport"]),
            int(row["dport"]), int(row["pg"]))


def completion_times(fct_rows: list[dict[str, str]]) -> dict[tuple[int, int, int, int, int], float]:
    result: dict[tuple[int, int, int, int, int], float] = {}
    for row in fct_rows:
        if not row.get("fct_ns", "").isdigit():
            continue
        key = flow_key(row)
        end_s = (int(row["start_time_ns"]) + int(row["fct_ns"])) / 1e9
        result[key] = max(result.get(key, 0.0), end_s)
    return result


def stages_for(flow: FlowSpec, flows: list[FlowSpec], completed: dict[tuple[int, int, int, int, int], float],
               stop_s: float) -> list[tuple[float, float, int]]:
    end_times = {item.key: completed.get(item.key, stop_s) for item in flows}
    events = {stop_s}
    events.update(item.start_s for item in flows if item.start_s < stop_s)
    events.update(end for end in end_times.values() if 0 < end < stop_s)
    ordered = sorted(events)
    stages: list[tuple[float, float, int]] = []
    for start, end in zip(ordered, ordered[1:]):
        if end <= start:
            continue
        active = [item for item in flows
                  if item.start_s <= start + 1e-12 and end_times[item.key] > start + 1e-12]
        if flow.key not in {item.key for item in active}:
            continue
        count = sum(item.direction == flow.direction for item in active)
        if count == 0:
            continue
        if stages and stages[-1][2] == count and abs(stages[-1][1] - start) < 1e-12:
            stages[-1] = (stages[-1][0], end, count)
        else:
            stages.append((start, end, count))
    return stages


def settling(samples: list[tuple[float, float]], start: float, end: float,
             target: float, tolerance: float, hold: float) -> float | None:
    usable = [(time_s, value) for time_s, value in samples if start <= time_s <= end]
    if not usable or end - hold < start:
        return None
    for index, (candidate, _) in enumerate(usable):
        if candidate < start or candidate + hold > end:
            continue
        window = [(time_s, value) for time_s, value in usable[index:] if time_s <= candidate + hold + 1e-12]
        if not window or window[-1][0] < candidate + hold - 1e-9:
            continue
        if all(target * (1 - tolerance) <= value <= target * (1 + tolerance)
               for _, value in window):
            return max(0.0, candidate - start)
    return None


def flow_metrics(rows: list[dict[str, str]], flows: list[FlowSpec],
                 completed: dict[tuple[int, int, int, int, int], float], stop_s: float,
                 rate_kind: str, rate_field: str, dci_rate: float,
                 packet_payload_size: int, data_header_bytes: int) -> list[dict[str, object]]:
    grouped: dict[tuple[str, str, str, str, str], list[tuple[float, float]]] = {}
    for row in rows:
        key = tuple(row[field] for field in ("src", "dst", "sport", "dport", "pg"))
        grouped.setdefault(key, []).append((int(row["time_ns"]) / 1e9, float(row[rate_field])))
    flow_by_key = {flow.key: flow for flow in flows}
    result = []
    for key, samples in grouped.items():
        sample_row = {field: value for field, value in zip(("src", "dst", "sport", "dport", "pg"), key)}
        flow = flow_by_key.get(flow_key(sample_row))
        if flow is None:
            raise SystemExit(f"rate row does not match flow file: {sample_row}")
        hold = max(3 * flow.base_rtt_ns / 1e9, 0.020)
        for stage_index, (start, end, count) in enumerate(stages_for(flow, flows, completed, stop_s)):
            path_capacity = min(flow.bottleneck_rate_bps, dci_rate) if flow.direction != "local" else flow.bottleneck_rate_bps
            target = path_capacity / count * packet_payload_size / (packet_payload_size + data_header_bytes)
            stage_samples = [(time_s, value) for time_s, value in samples if start <= time_s <= end]
            values = [value for _, value in stage_samples]
            metrics: dict[str, object] = {
                "stage": stage_index,
                "src": key[0], "dst": key[1], "sport": key[2], "dport": key[3], "pg": key[4],
                "target_bps": target,
                "base_rtt_ns": flow.base_rtt_ns,
                "path_hops": flow.path_hops,
                "bottleneck_rate_bps": flow.bottleneck_rate_bps,
                "bdp_bytes": flow.bdp_bytes,
                "window_bytes": flow.window_bytes,
                "sample_count": len(values),
            }
            for tolerance, label in ((0.05, "5"), (0.10, "10"), (0.20, "20")):
                value = settling(samples, start, end, target, tolerance, hold)
                metrics[f"{rate_kind}_settling_{label}_ms"] = "not_converged" if value is None else value * 1000
            stable = [value for time_s, value in stage_samples if time_s >= max(start, end - hold)]
            metrics[f"{rate_kind}_steady_avg_bps"] = mean(stable) if stable else float("nan")
            metrics[f"{rate_kind}_steady_p5_bps"] = quantile(stable, 0.05)
            metrics[f"{rate_kind}_steady_p50_bps"] = median(stable) if stable else float("nan")
            metrics[f"{rate_kind}_steady_p95_bps"] = quantile(stable, 0.95)
            metrics[f"{rate_kind}_overshoot_pct"] = max(0.0, max(values) - target) / target * 100 if values else float("nan")
            metrics[f"{rate_kind}_undershoot_pct"] = max(0.0, target - min(values)) / target * 100 if values else float("nan")
            metrics[f"{rate_kind}_cv"] = pstdev(stable) / mean(stable) if len(stable) > 1 and mean(stable) else float("nan")
            result.append(metrics)
    return result


def jain_index(rows: list[dict[str, str]]) -> dict[str, float]:
    by_time: dict[int, dict[tuple[str, str, str, str, str], float]] = {}
    for row in rows:
        key = tuple(row[field] for field in ("src", "dst", "sport", "dport", "pg"))
        by_time.setdefault(int(row["time_ns"]), {})[key] = float(row["goodput_bps"])
    values = []
    for rates in by_time.values():
        if len(rates) < 2:
            continue
        total = sum(rates.values())
        denominator = len(rates) * sum(value * value for value in rates.values())
        if denominator:
            values.append(total * total / denominator)
    return {"jain_min": min(values) if values else float("nan"),
            "jain_median": median(values) if values else float("nan"),
            "jain_steady": median(values[-max(1, len(values) // 4):]) if values else float("nan")}


def run_aggregate(dci_rows: list[dict[str, str]], fct_rows: list[dict[str, str]],
                  pfc_rows: list[dict[str, str]], rtt_rows: list[dict[str, str]], dci_rate: float,
                  hold_s: float, stop_s: float) -> dict[str, object]:
    dci_values = []
    queue_values = []
    ecn_values = []
    for row in dci_rows:
        time_s = int(row["time_ns"]) / 1e9
        if time_s >= max(0.0, stop_s - hold_s):
            dci_values.append(float(row["tx_bps"]) / dci_rate)
            queue_values.append(float(row["queue_bytes"]))
            ecn_values.append(int(row.get("ecn_events", 0)))
    fcts = [int(row["fct_ns"]) for row in fct_rows if row.get("fct_ns", "").isdigit()]
    standalone = [int(row["standalone_fct_ns"]) for row in fct_rows if row.get("standalone_fct_ns", "").isdigit()]
    normalized = [fct / base for fct, base in zip(fcts, standalone) if base]
    stable_rtts = [float(row["rtt_p50_ns"]) for row in rtt_rows
                   if int(row["time_ns"]) / 1e9 >= max(0.0, stop_s - hold_s)]
    stable_rtt_p95 = [float(row["rtt_p95_ns"]) for row in rtt_rows
                      if int(row["time_ns"]) / 1e9 >= max(0.0, stop_s - hold_s)]
    pause_events = sum(row.get("event_type") in ("1", "2") for row in pfc_rows)
    resume_events = sum(row.get("event_type") in ("0", "3") for row in pfc_rows)
    return {
        "dci_utilization_p50": median(dci_values) if dci_values else float("nan"),
        "dci_utilization_p95": quantile(dci_values, 0.95),
        "dci_queue_p50_bytes": median(queue_values) if queue_values else float("nan"),
        "dci_queue_p95_bytes": quantile(queue_values, 0.95),
        "dci_queue_p99_bytes": quantile(queue_values, 0.99),
        "dci_queue_max_bytes": max(queue_values) if queue_values else float("nan"),
        "ecn_events": max(ecn_values) if ecn_values else 0,
        "pfc_pause_events": pause_events,
        "pfc_resume_events": resume_events,
        "fct_count": len(fcts),
        "fct_p50_ns": median(fcts) if fcts else float("nan"),
        "fct_p95_ns": quantile(fcts, 0.95),
        "normalized_fct_p50": median(normalized) if normalized else float("nan"),
        "measured_rtt_p50_ns": median(stable_rtts) if stable_rtts else float("nan"),
        "measured_rtt_p95_ns": median(stable_rtt_p95) if stable_rtt_p95 else float("nan"),
    }


def required_metadata(metadata: dict[str, object], key: str, path: Path) -> object:
    if key not in metadata or metadata[key] in (None, ""):
        raise SystemExit(f"metadata field {key!r} is missing: {path}")
    return metadata[key]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("results/longhaul"))
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()
    output = args.output or args.root / "summary.csv"
    output.parent.mkdir(parents=True, exist_ok=True)
    run_metadata = sorted(args.root.glob("**/runner-metadata.json"))
    all_rows: list[dict[str, object]] = []
    for metadata_path in run_metadata:
        runner = json.loads(metadata_path.read_text())
        if runner.get("status") != "ok":
            continue
        run_dir = metadata_path.parent
        simulator_meta_path = run_dir / "metadata.json"
        if not simulator_meta_path.is_file():
            raise SystemExit(f"simulator metadata not found: {simulator_meta_path}")
        simulator_meta = json.loads(simulator_meta_path.read_text())
        scenario = str(required_metadata(simulator_meta, "scenario", simulator_meta_path)).upper()
        dci_rate = float(required_metadata(simulator_meta, "dci_rate_bps", simulator_meta_path))
        stop_s = float(required_metadata(simulator_meta, "simulator_stop_time_s", simulator_meta_path))
        dci_left = int(required_metadata(simulator_meta, "dci_left", simulator_meta_path))
        dci_right = int(required_metadata(simulator_meta, "dci_right", simulator_meta_path))
        topology_path = resolve_input(str(required_metadata(simulator_meta, "topology_file", simulator_meta_path)), run_dir)
        flow_path = resolve_input(str(required_metadata(simulator_meta, "flow_file", simulator_meta_path)), run_dir)
        sender = read_csv(run_dir / "sender-rate.csv")
        receiver = read_csv(run_dir / "receiver-goodput.csv")
        dci_rows = read_csv(run_dir / "dci-link.csv")
        rtt_rows = read_csv(run_dir / "measured-rtt.csv")
        fct_rows = read_csv(run_dir / "fct.csv")
        path_metrics = required_metadata(simulator_meta, "flow_path_metrics", simulator_meta_path)
        flows = read_flow_specs(flow_path, topology_path, dci_left, dci_right, path_metrics)
        completed = completion_times(fct_rows)
        packet_payload_size = int(required_metadata(simulator_meta, "packet_payload_size", simulator_meta_path))
        data_header_bytes = int(required_metadata(simulator_meta, "data_header_bytes", simulator_meta_path))
        sender_metrics = flow_metrics(sender, flows, completed, stop_s,
                                      "sender", "tx_payload_bps", dci_rate,
                                      packet_payload_size, data_header_bytes)
        receiver_metrics = flow_metrics(receiver, flows, completed, stop_s,
                                        "receiver", "goodput_bps", dci_rate,
                                        packet_payload_size, data_header_bytes)
        receiver_by_key = {(m["stage"], m["src"], m["dst"], m["sport"], m["dport"], m["pg"]): m for m in receiver_metrics}
        pfc_rows = read_csv(run_dir / "pfc.csv")
        jain = jain_index(receiver)
        max_base_rtt_s = max((flow.base_rtt_ns for flow in flows), default=0) / 1e9
        aggregate = run_aggregate(dci_rows, fct_rows, pfc_rows, rtt_rows, dci_rate,
                                  max(3 * max_base_rtt_s, 0.020),
                                  stop_s)
        for metric in sender_metrics:
            key = (metric["stage"], metric["src"], metric["dst"], metric["sport"], metric["dport"], metric["pg"])
            receiver_metric = receiver_by_key.get(key, {})
            row = {
                "algorithm": required_metadata(simulator_meta, "algorithm", simulator_meta_path),
                "cc_mode": required_metadata(simulator_meta, "cc_mode", simulator_meta_path),
                "scenario": scenario,
                "seed": required_metadata(simulator_meta, "rng_seed", simulator_meta_path),
                "run": required_metadata(simulator_meta, "rng_run", simulator_meta_path), **metric,
                **{key_name: value for key_name, value in receiver_metric.items()
                   if key_name.startswith("receiver_")},
                "observation_lag_ms": (
                    float(receiver_metric["receiver_settling_10_ms"]) - float(metric["sender_settling_10_ms"])
                    if receiver_metric.get("receiver_settling_10_ms") not in (None, "not_converged")
                    and metric.get("sender_settling_10_ms") not in (None, "not_converged") else "not_converged"),
                "pfc_event_count": len(pfc_rows), **aggregate, **jain,
                "wall_clock_seconds": runner.get("wall_clock_seconds", "nan"),
                "run_dir": str(run_dir),
            }
            all_rows.append(row)
    if not all_rows:
        raise SystemExit(f"no successful run data found below {args.root}")
    fields = list(all_rows[0])
    with output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(all_rows)
    run_output = output.parent / "run-summary.csv"
    run_rows = []
    run_fields = (
        "algorithm", "cc_mode", "scenario", "seed", "run", "sender_settling_10_ms",
        "receiver_settling_10_ms", "observation_lag_ms", "dci_utilization_p50",
        "dci_utilization_p95", "dci_queue_p50_bytes", "dci_queue_p95_bytes",
        "dci_queue_p99_bytes", "dci_queue_max_bytes", "ecn_events", "pfc_pause_events",
        "pfc_resume_events", "fct_count", "fct_p50_ns", "fct_p95_ns",
        "measured_rtt_p50_ns", "measured_rtt_p95_ns",
        "normalized_fct_p50", "jain_min", "jain_median", "jain_steady",
        "wall_clock_seconds", "run_dir")
    settling_fields = {"sender_settling_10_ms", "receiver_settling_10_ms", "observation_lag_ms"}
    grouped_runs: dict[tuple[object, ...], list[dict[str, object]]] = {}
    for row in all_rows:
        identity = (row["algorithm"], row["scenario"], row["seed"], row["run"])
        grouped_runs.setdefault(identity, []).append(row)
    for group in grouped_runs.values():
        first = group[0]
        run_row = {}
        for key in run_fields:
            if key not in first:
                continue
            if key in settling_fields:
                numeric = []
                for item in group:
                    try:
                        value = float(item[key])
                        if math.isfinite(value):
                            numeric.append(value)
                    except (TypeError, ValueError):
                        pass
                run_row[key] = median(numeric) if numeric else "not_converged"
            else:
                run_row[key] = first[key]
        run_rows.append(run_row)
    with run_output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(run_rows[0]))
        writer.writeheader()
        writer.writerows(run_rows)
    print(f"wrote {len(all_rows)} per-flow stage rows to {output}")
    print(f"wrote {len(run_rows)} run rows to {run_output}")
    subprocess.run([sys.executable, str(Path(__file__).with_name("plot-longhaul.py")),
                    "--root", str(args.root), "--summary", str(output),
                    "--all-scenarios"], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
