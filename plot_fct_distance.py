#!/usr/bin/env python3
"""
Plot average FCT across different distances for five RDMA congestion control algorithms.
Data is taken from the experimental result table.
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# Experimental data
distances = ["200 km", "400 km", "600 km","800 km"]
algorithms = ["DCQCN", "HPCC", "TIMELY", "FRP", "ROCC"]

# Rows: distance; columns: algorithm in the order above
fct_data = np.array([
    [1758.98, 2534.52, 2280.11, 1450.12, 1406.17],
    [2668.47, 4379.74, 3159.77, 2535.77, 2424.82],
    [3712.46, 4290.53, 4243.15, 3580.87, 3473.06],
    [4790.36, 5219.56, 5237.9, 4618.76, 4534.6]
])

# Plot settings
x = np.arange(len(distances))
width = 0.15
colors = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd"]

fig, ax = plt.subplots(figsize=(10, 6))

# Draw grouped bars for each algorithm
for i, algo in enumerate(algorithms):
    offset = (i - 2) * width
    bars = ax.bar(x + offset, fct_data[:, i], width, label=algo, color=colors[i])
    # Add value labels on top of each bar
    for bar in bars:
        height = bar.get_height()
        ax.annotate(f"{height:.1f}",
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha="center", va="bottom",
                    fontsize=9)

# Axis and labels
ax.set_xlabel("Distance", fontsize=14)
ax.set_ylabel("Average FCT (us)", fontsize=14)
ax.set_title("Average FCT vs. Distance", fontsize=16)
ax.set_xticks(x)
ax.set_xticklabels(distances, fontsize=12)
ax.legend(loc="upper left", fontsize=12)
ax.grid(axis="y", linestyle="--", alpha=0.6)

plt.tight_layout()

# Save figure
output_path = "results/fct_distance_comparison.png"
plt.savefig(output_path, dpi=300, bbox_inches="tight")
print(f"Figure saved to: {output_path}")

plt.show()
