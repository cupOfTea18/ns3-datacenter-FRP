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
    "DCQCN": [4.806, 4.270, 5.565, 6.866, 12.304],
    "HPCC":  [6.117, 6.829, 12.951, 10.565, 33.162],
    "TIMELY":[7.072, 8.274, 12.350, 17.769, 32.089],
    "FRP":   [2.924, 3.968, 5.830, 7.244, 10.421],
    "ROCC":  [3.783, 4.817, 6.470, 6.019, 6.759],
    "ATC":   [4.806, 7.706, 17.370, 25.756, 25.800],
}

# 表格数据：P99 FCT
p99_fct = {
    "DCQCN": [6.911, 7.080, 8.473, 10.753, 14.878],
    "HPCC":  [9.509, 9.777, 25.915, 18.957, 48.631],
    "TIMELY":[13.764, 16.300, 26.990, 34.423, 53.194],
    "FRP":   [4.210, 6.736, 9.170, 9.196, 14.083],
    "ROCC":  [4.613, 7.192, 8.650, 9.065, 11.678],
    "ATC":   [6.911, 20.828, 39.170, 37.967, 37.581],
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
