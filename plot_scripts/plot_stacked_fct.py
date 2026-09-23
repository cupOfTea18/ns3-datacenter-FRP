#!/usr/bin/env python3
"""
根据表格数据绘制 Average + Tail (P99) FCT 堆叠柱状图示例。
表格中包含 6 种算法在 5 个负载水平下的平均 FCT 与 P99 FCT。
"""

import matplotlib.pyplot as plt
import numpy as np

# 负载水平
loads = ["0%", "10%", "30%", "50%", "70%"]
x = np.arange(len(loads))

# 表格数据：平均 FCT
avg_fct = {
    "DCQCN": [4.808, 4.271, 5.560, 5.645, 12.300],
    "HPCC":  [3.624, 5.317, 9.496, 9.635, 32.735],
    "TIMELY":[7.190, 8.313, 14.416, 11.082, 26.150],
    "FRP":   [2.920, 3.968, 5.834, 6.897, 10.286],
    "ROCC":  [3.788, 4.816, 6.133, 7.122, 8.258],
    "ATC":   [4.808, 6.311, 8.674, 7.630, 9.665],
}

# 表格数据：P99 FCT
p99_fct = {
    "DCQCN": [6.911, 7.080, 8.476, 10.351, 14.876],
    "HPCC":  [7.551, 9.252, 22.528, 20.848, 52.889],
    "TIMELY":[14.012, 16.331, 35.248, 21.434, 44.270],
    "FRP":   [4.210, 6.736, 9.164, 10.806, 13.952],
    "ROCC":  [4.613, 7.191, 7.272, 10.042, 13.629],
    "ATC":   [6.911, 11.071, 12.538, 11.777, 12.873],
}

algorithms = ["DCQCN", "HPCC", "TIMELY", "FRP", "ROCC", "ATC"]
colors = {
    "DCQCN": "#1f77b4",
    "HPCC":  "#9467bd",
    "TIMELY":"#2ca02c",
    "FRP":   "#d62728",
    "ROCC":  "#e377c2",
    "ATC":   "#8c564b",
}

bar_width = 0.12
# 每组 6 个柱子，整体居中偏移
offsets = np.linspace(-2.5 * bar_width, 2.5 * bar_width, len(algorithms))

fig, ax = plt.subplots(figsize=(12, 6))

for i, algo in enumerate(algorithms):
    avg = np.array(avg_fct[algo])
    p99 = np.array(p99_fct[algo])
    pos = x + offsets[i]
    # 底部：Average FCT
    ax.bar(
        pos,
        avg,
        bar_width,
        label=f"{algo} Average",
        color=colors[algo],
        edgecolor="black",
        linewidth=0.5,
    )
    # 顶部堆叠：P99 FCT
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

ax.set_xlabel("Load Level", fontsize=13)
ax.set_ylabel("Latency (ms)", fontsize=13)
ax.set_title("Average and Tail Latency of Different Algorithms Under Varying Loads", fontsize=14)
ax.set_xticks(x)
ax.set_xticklabels(loads)
ax.legend(ncol=3, fontsize=9, loc="upper left")
ax.grid(axis="y", linestyle="--", alpha=0.4)

plt.tight_layout()
# plt.savefig("results/stacked_fct_example.png", dpi=300, bbox_inches="tight")
plt.show()
