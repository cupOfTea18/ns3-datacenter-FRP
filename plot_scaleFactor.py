#!/usr/bin/env python3
"""
Plot FCT and queue length curves against scaleFactor.
Data is taken from the experimental result table.
"""

import matplotlib.pyplot as plt
import numpy as np

# Experimental data
scale_factors = [5, 10, 15, 20, 25, 30, 35, 40]

avg_fct = [2105.64, 2041.34, 2014.75, 2001.23, 1975.63, 1932.10, 1925.46, 1921.29]
p99_fct = [4789.81, 4406.27, 4235.82, 4164.60, 4047.22, 3922.33, 3887.77, 3854.11]

avg_qlen = [658, 439, 336, 292, 203, 304, 314, 291]
max_qlen = [3538, 3623, 3080, 3204, 3142, 5535, 4364, 4532]

x = np.array(scale_factors)

fig, axes = plt.subplots(1, 2, figsize=(14, 5))

# Figure 1: Flow Completion Time
ax = axes[0]
ax.plot(x, avg_fct, marker="o", linewidth=2, markersize=7, label="Average FCT")
ax.plot(x, p99_fct, marker="s", linewidth=2, markersize=7, label="P99 FCT")
ax.set_xlabel("scaleFactor", fontsize=14)
ax.set_ylabel("Flow Completion Time (us)", fontsize=14)
ax.set_title("Impact of scaleFactor on FCT", fontsize=16)
ax.set_xticks(x)
ax.legend(fontsize=12)
ax.grid(True, linestyle="--", alpha=0.6)

# Figure 2: Queue Length
ax = axes[1]
ax.plot(x, avg_qlen, marker="o", linewidth=2, markersize=7, label="Average Queue Length")
ax.plot(x, max_qlen, marker="^", linewidth=2, markersize=7, label="Maximum Queue Length")
ax.set_xlabel("scaleFactor", fontsize=14)
ax.set_ylabel("Queue Length (KB)", fontsize=14)
ax.set_title("Impact of scaleFactor on Queue Length", fontsize=16)
ax.set_xticks(x)
ax.legend(fontsize=12)
ax.grid(True, linestyle="--", alpha=0.6)

plt.tight_layout()


plt.show()
