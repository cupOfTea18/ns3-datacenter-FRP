#!/usr/bin/env python3
"""Analyze longhaul CSVs with one common settling-time definition."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean, median, pstdev


STAGES = {
    "S0": [(0.01, 1, 0.38, "all")],
    "S1": [(0.01, 8, 0.38, "all")],
    "S2": [(0.01, 1, 0.21, "first"), (0.21, 8, 0.60, "late")],
    "S3": [(0.01, 8, 0.21, "finite"), (0.21, 1, 0.60, "survivor")],
    "S4": [(0.01, 8, 0.38, "direction")],
    "S5": [(0.01, 8, 0.38, "long")],
}


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


def flow_role(scenario: str, row: dict[str, str]) -> str:
    port = int(row["dport"])
    if scenario == "S2":
        return "first" if port == 20000 else "late"
    if scenario == "S3":
        return "finite" if port <= 20006 else "survivor"
    if scenario == "S5":
        return "long" if port < 20100 else "probe"
    return "all"


def stages_for(scenario: str, row: dict[str, str]) -> list[tuple[float, int, float]]:
    role = flow_role(scenario, row)
    return [(start, count, end) for start, count, end, stage_role in STAGES[scenario]
            if stage_role == "all" or stage_role == role or
            (stage_role == "direction" and role == "all")]


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


def flow_metrics(rows: list[dict[str, str]], scenario: str, rate_kind: str,
                 dci_rate: float, nic_rate: float, base_rtt_s: float) -> list[dict[str, object]]:
    grouped: dict[tuple[str, str, str, str, str], list[tuple[float, float]]] = {}
    for row in rows:
        key = tuple(row[field] for field in ("src", "dst", "sport", "dport", "pg"))
        grouped.setdefault(key, []).append((int(row["time_ns"]) / 1e9, float(row["value_bps"])))
    result = []
    hold = max(3 * base_rtt_s, 0.020)
    for key, samples in grouped.items():
        sample_row = {field: value for field, value in zip(("src", "dst", "sport", "dport", "pg"), key)}
        for stage_index, (start, count, end) in enumerate(stages_for(scenario, sample_row)):
            # The target is the narrowest path bottleneck, not always the DCI:
            # every path also crosses a 100 Gbps host access link.
            target = min(dci_rate / count, nic_rate)
            stage_samples = [(time_s, value) for time_s, value in samples if start <= time_s <= end]
            values = [value for _, value in stage_samples]
            metrics: dict[str, object] = {
                "stage": stage_index,
                "src": key[0], "dst": key[1], "sport": key[2], "dport": key[3], "pg": key[4],
                "target_bps": target,
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


def jain_index(rows: list[dict[str, str]], scenario: str, dci_rate: float) -> dict[str, float]:
    by_time: dict[int, dict[tuple[str, str, str, str, str], float]] = {}
    for row in rows:
        key = tuple(row[field] for field in ("src", "dst", "sport", "dport", "pg"))
        by_time.setdefault(int(row["time_ns"]), {})[key] = float(row["value_bps"])
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
                  pfc_rows: list[dict[str, str]], dci_rate: float,
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
    }


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
        simulator_meta = json.loads(simulator_meta_path.read_text()) if simulator_meta_path.is_file() else {}
        scenario = str(runner["scenario"]).upper()
        base_rtt_s = float(simulator_meta.get("representative_rtt_ns", 10e6)) / 1e9
        dci_rate = float(simulator_meta.get("dci_rate_bps", 200e9))
        sender = read_csv(run_dir / "sender-rate.csv")
        receiver = read_csv(run_dir / "receiver-goodput.csv")
        dci_rows = read_csv(run_dir / "dci-link.csv")
        fct_rows = read_csv(run_dir / "fct.csv")
        nic_rate = float(simulator_meta.get("nic_rate_bps", 100e9))
        sender_metrics = flow_metrics(sender, scenario, "sender", dci_rate, nic_rate, base_rtt_s)
        receiver_metrics = flow_metrics(receiver, scenario, "receiver", dci_rate, nic_rate, base_rtt_s)
        receiver_by_key = {(m["stage"], m["src"], m["dst"], m["sport"], m["dport"], m["pg"]): m for m in receiver_metrics}
        pfc_rows = read_csv(run_dir / "pfc.csv")
        jain = jain_index(receiver, scenario, dci_rate)
        aggregate = run_aggregate(dci_rows, fct_rows, pfc_rows, dci_rate,
                                  max(3 * base_rtt_s, 0.020),
                                  float(simulator_meta.get("simulator_stop_time_s", STAGES[scenario][-1][2])))
        for metric in sender_metrics:
            key = (metric["stage"], metric["src"], metric["dst"], metric["sport"], metric["dport"], metric["pg"])
            receiver_metric = receiver_by_key.get(key, {})
            row = {
                "algorithm": runner["algorithm"], "cc_mode": runner["cc_mode"], "scenario": scenario,
                "seed": runner["seed"], "run": runner["run"], **metric,
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
    seen = set()
    for row in all_rows:
        identity = (row["algorithm"], row["scenario"], row["seed"], row["run"])
        if identity in seen:
            continue
        seen.add(identity)
        run_rows.append({key: row[key] for key in (
            "algorithm", "cc_mode", "scenario", "seed", "run", "sender_settling_10_ms",
            "receiver_settling_10_ms", "observation_lag_ms", "dci_utilization_p50",
            "dci_utilization_p95", "dci_queue_p50_bytes", "dci_queue_p95_bytes",
            "dci_queue_p99_bytes", "dci_queue_max_bytes", "ecn_events", "pfc_pause_events",
            "pfc_resume_events", "fct_count", "fct_p50_ns", "fct_p95_ns",
            "normalized_fct_p50", "jain_min", "jain_median", "jain_steady",
            "wall_clock_seconds", "run_dir") if key in row})
    with run_output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(run_rows[0]))
        writer.writeheader()
        writer.writerows(run_rows)
    print(f"wrote {len(all_rows)} per-flow stage rows to {output}")
    print(f"wrote {len(run_rows)} run rows to {run_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
