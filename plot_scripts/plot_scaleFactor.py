#!/usr/bin/env python3
"""
Plot stacked bar charts for FCT and queue length against scaleFactor.
Data is taken from the experimental result table.
"""

import matplotlib.pyplot as plt
import numpy as np

# Experimental data
scale_factors = [5, 10, 12, 15, 20, 25, 30, 35, 40]
x = np.arange(len(scale_factors))

avg_fct = np.array([4.09, 4.006, 3.968, 3.969, 4.112, 4.337, 4.496, 4.837, 4.613])
p99_fct = np.array([7.212, 6.867, 6.736, 6.946, 6.365, 7.891, 8.079, 9.829, 8.489])

avg_qlen = np.array([3115.3, 2211.7, 1509.4, 1761.4, 1347.9, 1059.4, 965.6, 837.0, 715.8])
max_qlen = np.array([13030.0, 11619.1, 8467.9, 9410.5, 7082.2, 5954.4, 5239.0, 4678.1, 4252.4])


def add_total_labels(ax, bars, totals, fmt="{:.3f}", padding=0.02):
    ylim_top = ax.get_ylim()[1]
    offset = ylim_top * padding
    for bar, total in zip(bars, totals):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            total + offset,
            fmt.format(total),
            ha="center",
            va="bottom",
            fontsize=9,
        )


def plot_stacked_bars(x_positions, labels, base, total, ylabel, title, base_label, extra_label, label_fmt, color):
    fig, ax = plt.subplots(figsize=(10, 5.6))
    bar_width = 0.58
    extra = total - base

    base_bars = ax.bar(
        x_positions,
        base,
        bar_width,
        label=base_label,
        color=color,
        edgecolor="black",
        linewidth=0.5,
    )
    ax.bar(
        x_positions,
        extra,
        bar_width,
        bottom=base,
        label=extra_label,
        color=color,
        edgecolor="black",
        linewidth=0.5,
        hatch="//",
        alpha=0.9,
    )

    ax.set_xlabel("scaleFactor", fontsize=13)
    ax.set_ylabel(ylabel, fontsize=13)
    ax.set_title(title, fontsize=15)
    ax.set_xticks(x_positions)
    ax.set_xticklabels(labels)
    ax.grid(axis="y", linestyle="--", alpha=0.45)
    ax.legend(fontsize=11)
    ax.set_axisbelow(True)
    ax.set_ylim(0, total.max() * 1.14)
    add_total_labels(ax, base_bars, total, fmt=label_fmt)
    fig.tight_layout()
    return fig


plot_stacked_bars(
    x,
    scale_factors,
    avg_fct,
    p99_fct,
    "FCT (ms)",
    "Average and P99 FCT vs. scaleFactor",
    "Average FCT",
    "P99 FCT",
    "{:.3f}",
    "#1f77b4",
)

plot_stacked_bars(
    x,
    scale_factors,
    avg_qlen,
    max_qlen,
    "Queue Length (KB)",
    "Average and Maximum Queue Length vs. scaleFactor",
    "Average Queue Length",
    "Maximum Queue Length",
    "{:.1f}",
    "#F58518",
)

plt.show()
