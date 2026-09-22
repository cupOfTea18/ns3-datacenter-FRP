#!/usr/bin/env python3
"""Generate the six baseline figures from longhaul CSV output."""

from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean, median


def rows_below(root: Path, filename: str) -> list[tuple[dict[str, str], Path]]:
    rows = []
    for path in root.glob(f"**/{filename}"):
        with path.open(newline="") as stream:
            rows.extend((row, path.parent) for row in csv.DictReader(stream))
    return rows


def read_summary(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def save(fig, path: Path) -> None:
    import matplotlib.pyplot as plt

    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)


def plot_rates(root: Path, out: Path, scenario: str, filename: str, title: str) -> None:
    import matplotlib.pyplot as plt

    per_run: dict[tuple[str, str, str, int], float] = defaultdict(float)
    for row, _ in rows_below(root, filename):
        # Raw rate CSVs intentionally have no scenario field; root is already
        # restricted to the requested scenario directory by the caller.
        per_run[(row["algorithm"], row["seed"], row["run"], int(row["time_ns"]))] += float(row["value_bps"])
    grouped: dict[tuple[str, int], list[float]] = defaultdict(list)
    for (algorithm, _, _, time_ns), value in per_run.items():
        grouped[(algorithm, time_ns)].append(value)
    fig, axis = plt.subplots(figsize=(8, 4.5))
    for algorithm in ("dcqcn", "hpcc", "timely"):
        points = sorted((time_ns / 1e6, mean(values) / 1e9)
                        for (name, time_ns), values in grouped.items() if name == algorithm)
        if points:
            axis.plot([point[0] for point in points], [point[1] for point in points], label=algorithm)
    axis.set_xlabel("simulation time (ms)")
    axis.set_ylabel("aggregate rate (Gbps)")
    axis.set_title(f"{scenario}: {title}")
    axis.grid(alpha=0.25)
    if axis.get_legend_handles_labels()[0]:
        axis.legend()
    save(fig, out / f"{scenario.lower()}-{filename.removesuffix('.csv')}.png")


def plot_settling(summary: list[dict[str, str]], out: Path, scenario: str) -> None:
    import matplotlib.pyplot as plt

    fig, axis = plt.subplots(figsize=(8, 4.5))
    algorithms = ["dcqcn", "hpcc", "timely"]
    for offset, metric in enumerate(("sender_settling_10_ms", "receiver_settling_10_ms")):
        values = []
        for algorithm in algorithms:
            numeric = [float(row[metric]) for row in summary
                       if row["scenario"] == scenario and row["algorithm"] == algorithm
                       and row[metric] not in ("not_converged", "nan", "NaN")]
            values.append(median(numeric) if numeric else math.nan)
        positions = [index + (offset - 0.5) * 0.32 for index in range(len(algorithms))]
        axis.bar(positions, values, width=0.30, label=metric.replace("_", " "))
    axis.set_xticks(range(len(algorithms)), algorithms)
    axis.set_ylabel("settling time (ms)")
    axis.set_title(f"{scenario}: sender/receiver settling time (±10%)")
    if axis.get_legend_handles_labels()[0]:
        axis.legend()
    axis.grid(axis="y", alpha=0.25)
    save(fig, out / f"{scenario.lower()}-settling.png")


def plot_dci(root: Path, out: Path, scenario: str) -> None:
    import matplotlib.pyplot as plt

    rates: dict[tuple[str, int, str], list[float]] = defaultdict(list)
    queues: dict[tuple[str, int], list[float]] = defaultdict(list)
    for row, _ in rows_below(root, "dci-link.csv"):
        time_ns = int(row["time_ns"])
        key = (row["algorithm"], time_ns, row["direction"])
        rates[key].append(float(row["tx_bps"]))
        queues[(row["algorithm"], time_ns)].append(float(row["queue_bytes"]))
    fig, (rate_axis, queue_axis) = plt.subplots(2, 1, figsize=(8, 6), sharex=True)
    for algorithm in ("dcqcn", "hpcc", "timely"):
        for direction in ("left-to-right", "right-to-left"):
            points = sorted((time_ns / 1e6, mean(values) / 1e9)
                            for (name, time_ns, direct), values in rates.items()
                            if name == algorithm and direct == direction)
            if points:
                rate_axis.plot([x for x, _ in points], [y for _, y in points], label=f"{algorithm} {direction}")
        points = sorted((time_ns / 1e6, mean(values) / 1e6)
                        for (name, time_ns), values in queues.items() if name == algorithm)
        if points:
            queue_axis.plot([x for x, _ in points], [y for _, y in points], label=algorithm)
    rate_axis.set_ylabel("DCI TX (Gbps)")
    queue_axis.set_ylabel("queue (MB)")
    queue_axis.set_xlabel("simulation time (ms)")
    rate_axis.set_title(f"{scenario}: DCI utilization and queue")
    rate_axis.grid(alpha=0.25); queue_axis.grid(alpha=0.25)
    if rate_axis.get_legend_handles_labels()[0]:
        rate_axis.legend(ncol=2, fontsize=8)
    if queue_axis.get_legend_handles_labels()[0]:
        queue_axis.legend()
    save(fig, out / f"{scenario.lower()}-dci-link.png")


def plot_fairness(root: Path, out: Path, scenario: str) -> None:
    import matplotlib.pyplot as plt

    by_run_time: dict[tuple[str, str, str, int], dict[tuple[str, str, str, str, str], float]] = defaultdict(dict)
    for row, _ in rows_below(root, "receiver-goodput.csv"):
        key = (row["algorithm"], row["seed"], row["run"], int(row["time_ns"]))
        flow = tuple(row[field] for field in ("src", "dst", "sport", "dport", "pg"))
        by_run_time[key][flow] = float(row["value_bps"])
    grouped: dict[tuple[str, int], list[float]] = defaultdict(list)
    for (algorithm, _, _, time_ns), flow_rates in by_run_time.items():
        values = list(flow_rates.values())
        if len(values) < 2 or not sum(value * value for value in values):
            continue
        jain = sum(values) ** 2 / (len(values) * sum(value * value for value in values))
        grouped[(algorithm, time_ns)].append(jain)
    fig, axis = plt.subplots(figsize=(8, 4.5))
    for algorithm in ("dcqcn", "hpcc", "timely"):
        points = sorted((time_ns / 1e6, median(values))
                        for (name, time_ns), values in grouped.items() if name == algorithm)
        if points:
            axis.plot([x for x, _ in points], [y for _, y in points], label=algorithm)
    axis.set_ylim(0, 1.05); axis.set_xlabel("simulation time (ms)"); axis.set_ylabel("Jain fairness")
    axis.set_title(f"{scenario}: instantaneous receiver fairness"); axis.grid(alpha=0.25)
    if axis.get_legend_handles_labels()[0]:
        axis.legend()
    save(fig, out / f"{scenario.lower()}-fairness.png")


def plot_summary(summary: list[dict[str, str]], out: Path, scenario: str) -> None:
    import matplotlib.pyplot as plt

    algorithms = ["dcqcn", "hpcc", "timely"]
    fig, (throughput_axis, pfc_axis) = plt.subplots(1, 2, figsize=(9, 4.5))
    for algorithm in algorithms:
        rows = [row for row in summary if row["scenario"] == scenario and row["algorithm"] == algorithm]
        rates = [float(row["sender_steady_avg_bps"]) / 1e9 for row in rows if row["sender_steady_avg_bps"] not in ("nan", "NaN")]
        pfc = [float(row["pfc_event_count"]) for row in rows]
        index = algorithms.index(algorithm)
        throughput_axis.bar(index, median(rates) if rates else math.nan, label=algorithm)
        pfc_axis.bar(index, median(pfc) if pfc else math.nan, label=algorithm)
    throughput_axis.set_xticks(range(3), algorithms); throughput_axis.set_ylabel("steady rate (Gbps)")
    pfc_axis.set_xticks(range(3), algorithms); pfc_axis.set_ylabel("PFC events")
    throughput_axis.set_title(f"{scenario}: steady throughput"); pfc_axis.set_title(f"{scenario}: PFC")
    throughput_axis.grid(axis="y", alpha=0.25); pfc_axis.grid(axis="y", alpha=0.25)
    save(fig, out / f"{scenario.lower()}-summary.png")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("results/longhaul"))
    parser.add_argument("--output-dir", type=Path, default=None)
    parser.add_argument("--scenario", default="S0")
    parser.add_argument("--all-scenarios", action="store_true")
    args = parser.parse_args()
    out = args.output_dir or args.root / "plots"
    out.mkdir(parents=True, exist_ok=True)
    summary_path = args.root / "summary.csv"
    summary = read_summary(summary_path)
    scenarios = sorted({row["scenario"] for row in summary}) if args.all_scenarios else [args.scenario.upper()]
    if not summary:
        raise SystemExit(f"summary file not found or empty: {summary_path}")
    for scenario in scenarios:
        plot_rates(args.root / scenario.lower(), out, scenario, "sender-rate.csv", "sender actual rate")
        plot_rates(args.root / scenario.lower(), out, scenario, "receiver-goodput.csv", "receiver goodput")
        plot_settling(summary, out, scenario)
        plot_dci(args.root / scenario.lower(), out, scenario)
        plot_fairness(args.root / scenario.lower(), out, scenario)
        plot_summary(summary, out, scenario)
    print(f"wrote plots to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
