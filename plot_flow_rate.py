#!/usr/bin/env python3
"""
根据表格数据绘制两幅柱状图：
1) 各方案的平均流速率
2) 各方案的流速率方差
"""

import matplotlib.pyplot as plt
import numpy as np

# 方案与数据
algorithms = ["DCQCN", "HPCC", "TIMELY", "FRP", "ROCC", "ATC"]
avg_rate = [2521.308882, 1881.976739, 1576.575102, 3394.557885, 2140.17527, 1964.993383]
var_rate = [563965.2552, 1610575.38, 741430.4954, 5035323.752, 379082.8657, 1097570.373]

colors = {
    "DCQCN": "#1f77b4",
    "HPCC":  "#9467bd",
    "TIMELY":"#2ca02c",
    "FRP":   "#d62728",
    "ROCC":  "#e377c2",
    "ATC":   "#8c564b",
}


def plot_bar_chart(values, ylabel, title, output_path, color_list, value_fmt="{:.2f}"):
    fig, ax = plt.subplots(figsize=(8, 5))
    x = np.arange(len(algorithms))
    bar_width = 0.6

    bars = ax.bar(
        x,
        values,
        bar_width,
        color=color_list,
        edgecolor="black",
        linewidth=0.6,
    )

    # 在柱子上方标注数值
    for bar in bars:
        height = bar.get_height()
        ax.annotate(
            value_fmt.format(height),
            xy=(bar.get_x() + bar.get_width() / 2, height),
            xytext=(0, 3),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    ax.set_xlabel("Algorithm", fontsize=13)
    ax.set_ylabel(ylabel, fontsize=13)
    ax.set_title(title, fontsize=15)
    ax.set_xticks(x)
    ax.set_xticklabels(algorithms, fontsize=11)
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    ax.set_axisbelow(True)

    plt.tight_layout()
    plt.savefig(output_path, dpi=300, bbox_inches="tight")
    print(f"Figure saved to: {output_path}")
    return fig


# 颜色按算法顺序排列
bar_colors = [colors[algo] for algo in algorithms]

# 图 1：平均流速率
fig1 = plot_bar_chart(
    avg_rate,
    "Average Flow Rate",
    "Average Flow Rate by Algorithm",
    "results/avg_flow_rate.png",
    bar_colors,
    value_fmt="{:.2f}",
)

# 图 2：流速率方差
fig2 = plot_bar_chart(
    var_rate,
    "Flow Rate Variance",
    "Flow Rate Variance by Algorithm",
    "results/var_flow_rate.png",
    bar_colors,
    value_fmt="{:.2f}",
)

plt.show()
