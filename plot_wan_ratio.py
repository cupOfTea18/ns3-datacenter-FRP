#!/usr/bin/env python3
"""
根据广域流比例对比不同 RDMA 拥塞控制算法的性能。
数据来自实验结果表格。
"""

import matplotlib.pyplot as plt
import numpy as np

# 数据
wan_ratios = ["0% WAN", "20% WAN", "50% WAN", "80% WAN"]
algorithms = ["DCQCN", "HPCC", "TIMELY", "ROCC"]

# 每行对应一个广域流比例，每列对应一个算法
data = np.array([
    [709.9, 740.3, 717.5, 771.4],
    [590.8, 615.3, 596.9, 675.7],
    [424.5, 442.2, 428.9, 426.4],
    [232.0, 240.1, 234.1, 232.0],
])

# 绘图设置
x = np.arange(len(wan_ratios))  # 横坐标位置
width = 0.15                    # 每个柱子的宽度

colors = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd"]

fig, ax = plt.subplots(figsize=(10, 6))

# 绘制每个算法的柱状图
for i, algo in enumerate(algorithms):
    offset = (i - 2) * width
    bars = ax.bar(x + offset, data[:, i], width, label=algo, color=colors[i])
    # 在柱子上方标注数值
    for bar in bars:
        height = bar.get_height()
        ax.annotate(f"{height:.1f}",
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha="center", va="bottom",
                    fontsize=12)

# 设置坐标轴和标签
ax.set_xlabel("WAN Traffic Ratio", fontsize=20)
ax.set_ylabel("Transmission Delay (ms)", fontsize=20)
ax.set_title("FCT Across WAN Traffic Ratios", fontsize=20)
ax.set_xticks(x)
ax.set_xticklabels(wan_ratios, fontsize=18)
ax.legend(loc="upper right", ncol=len(algorithms), fontsize=18)

# 添加网格线便于对比
# ax.grid(axis="y", linestyle="--", alpha=0.7)

# 自动调整布局
plt.tight_layout()

# 保存图片
output_path = "wan_ratio_comparison.png"
# plt.savefig(output_path, dpi=300, bbox_inches="tight")
# print(f"图表已保存为: {output_path}")

# 显示图片（如果在交互环境中运行）
plt.show()
