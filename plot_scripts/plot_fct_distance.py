#!/usr/bin/env python3
"""Plot Average + Tail (P99) FCT across different distances for RDMA congestion control algorithms.
Data is taken from the experimental result table.
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# Experimental data
distances = ["200 km", "400 km", "600 km", "800 km", "1000 km"]
x = np.arange(len(distances))
algorithms = ["DCQCN", "HPCC", "TIMELY", "FRP", "ROCC", "ATC"]

# 平均 FCT
avg_fct = {
    "DCQCN": [4.270, 4.896, 5.066, 5.220, 5.380],
    "HPCC":  [6.829, 9.866, 7.270, 10.269, 12.326],
    "TIMELY":[8.274, 6.051, 6.256, 6.361, 6.544],
    "FRP":   [3.968, 4.830, 3.972, 3.706, 3.647],
    "ROCC":  [4.816, 4.881, 4.151, 4.150, 6.149],
    "ATC":   [6.292, 7.374, 6.415, 6.141, 6.057],
}

# P99 FCT
p99_fct = {
    "DCQCN": [7.080, 7.950, 9.732, 11.508, 13.287],
    "HPCC":  [9.777, 14.999, 14.590, 16.397, 17.863],
    "TIMELY":[16.300, 15.365, 15.379, 15.153, 15.392],
    "FRP":   [6.736, 10.355, 5.822, 6.407, 6.230],
    "ROCC":  [7.192, 7.173, 6.118, 6.123, 6.123],
    "ATC":   [10.992, 15.601, 12.527, 10.217, 9.531],
}

colors = {
    "DCQCN": "#1f77b4",
    "HPCC":  "#9467bd",
    "TIMELY":"#2ca02c",
    "FRP":   "#d62728",
    "ROCC":  "#e377c2",
    "ATC":   "#8c564b",
}

# Plot settings
bar_width = 0.12
offsets = np.linspace(-2.5 * bar_width, 2.5 * bar_width, len(algorithms))

fig, ax = plt.subplots(figsize=(12, 6))

# Draw stacked grouped bars for each algorithm
for i, algo in enumerate(algorithms):
    avg = np.array(avg_fct[algo])
    p99 = np.array(p99_fct[algo])
    pos = x + offsets[i]
    ax.bar(
        pos,
        avg,
        bar_width,
        label=f"{algo} Average",
        color=colors[algo],
        edgecolor="black",
        linewidth=0.5,
    )
    ax.bar(
        pos,
        p99,
        bar_width,
        bottom=avg,
        label=f"{algo} Tail",
        color=colors[algo],
        edgecolor="black",
        linewidth=0.5,
        hatch="//",
        alpha=0.85,
    )

# Axis and labels
ax.set_xlabel("Distance", fontsize=14)
ax.set_ylabel("FCT (ms)", fontsize=14)
ax.set_title("Average and Tail Latency vs. Distance", fontsize=16)
ax.set_xticks(x)
ax.set_xticklabels(distances, fontsize=12)
ax.legend(ncol=3, fontsize=9, loc="upper left")
ax.grid(axis="y", linestyle="--", alpha=0.6)

plt.tight_layout()

# Save figure
output_path = "results/fct_distance_comparison.png"
plt.savefig(output_path, dpi=300, bbox_inches="tight")
print(f"Figure saved to: {output_path}")
