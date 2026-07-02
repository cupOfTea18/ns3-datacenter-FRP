#!/usr/bin/env python3
"""
FCT 柱状图对比脚本
核心思路：读 5 个 FCT 文件 → 根据流大小过滤流 → 算平均/p99 → 画三张图。
只读已有结果，不触发仿真。
"""

import os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

FCT_DIR = "/home/shemuping/newCode/ns3-FRP/results/fct"
RESULTS_DIR = "/home/shemuping/newCode/ns3-FRP/results"
ALGOS = [("dcqcn", "DCQCN"), ("hpcc", "HPCC"), ("timely", "TIMELY"),
         ("frp", "FRP"), ("rocc", "ROCC")]
FLOW_SIZE = 10000000          # 过滤流阈值：m_size == 10000000 (10MB)
CROSS_DC_THRESHOLD_NS = 1000000   # standalone_fct > 1ms 视为 cross-DC


def parse_fct_file(filepath):
    """解析单个 FCT 文件，返回流列表。"""
    flows = []
    if not os.path.exists(filepath):
        print(f"[WARN] 文件不存在: {filepath}")
        return flows

    with open(filepath, 'r') as f:
        for line_no, line in enumerate(f, 1):
            parts = line.split()
            if len(parts) < 8:
                continue
            try:
                flows.append({
                    "size": int(parts[4]),
                    "fct_ns": int(parts[6]),
                    "standalone_ns": int(parts[7]),
                    "is_cross": int(parts[7]) > CROSS_DC_THRESHOLD_NS,
                })
            except ValueError:
                print(f"[WARN] 跳过无法解析的行: {filepath}:{line_no}")
    return flows


def select_flows(flows, algo_name):
    """只统计 m_size 小于 FLOW_SIZE 阈值的流。"""
    selected = [fl for fl in flows if fl["size"] < FLOW_SIZE]
    if not selected and flows:
        sizes = sorted({fl["size"] for fl in flows})
        size_text = ", ".join(str(s) for s in sizes)
        print(f"[WARN] {algo_name}: 没有 m_size < {FLOW_SIZE} 的流。可用 m_size: {size_text}")
    return selected


def percentile(values, p):
    """简单 p99 分位数（最近秩法）"""
    if not values:
        return float('nan')
    s = sorted(values)
    k = int(np.ceil(p / 100.0 * len(s)))
    k = min(max(k, 1), len(s))
    return s[k - 1]


def aggregate(flows):
    """对一组流计算平均 FCT 和 p99 FCT（μs）"""
    if not flows:
        return float('nan'), float('nan'), 0
    fcts = [fl["fct_ns"] for fl in flows]
    avg = np.mean(fcts) / 1000.0
    p99 = percentile(fcts, 99) / 1000.0
    return avg, p99, len(fcts)


def fmt(value):
    return "N/A" if np.isnan(value) else f"{value:.1f}"


def add_bar_labels(ax, bars, values, fontsize=9):
    for bar, val in zip(bars, values):
        if not np.isnan(val):
            ax.text(bar.get_x() + bar.get_width() / 2, val,
                    f'{val:.1f}', ha='center', va='bottom', fontsize=fontsize)


def save_bar_chart(filename, names, values, colors, ylabel, title):
    fig, ax = plt.subplots(figsize=(10, 6))
    plot_values = [0 if np.isnan(v) else v for v in values]
    bars = ax.bar(names, plot_values, color=colors)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    add_bar_labels(ax, bars, values)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS_DIR, filename), dpi=150)
    plt.close(fig)


def main():
    os.makedirs(RESULTS_DIR, exist_ok=True)

    algo_data = {}
    for fname, display in ALGOS:
        path = os.path.join(FCT_DIR, f"fct_{fname}.txt")
        flows = parse_fct_file(path)
        algo_data[display] = select_flows(flows, display)

    names = [display for _, display in ALGOS]
    avg_all, p99_all = [], []
    avg_intra, avg_cross = [], []
    has_intra, has_cross = [], []

    print("\n================ FCT 统计 ================")
    print(f"{'算法':<8} {'流数':<6} {'intra':<6} {'cross':<6} "
          f"{'avg(μs)':<12} {'p99(μs)':<12} "
          f"{'intra_avg(μs)':<14} {'cross_avg(μs)':<14}")
    for name in names:
        flows = algo_data[name]
        intra = [fl for fl in flows if not fl["is_cross"]]
        cross = [fl for fl in flows if fl["is_cross"]]
        a_all, p_all, n_all = aggregate(flows)
        a_in, _, n_in = aggregate(intra)
        a_cr, _, n_cr = aggregate(cross)
        avg_all.append(a_all)
        p99_all.append(p_all)
        avg_intra.append(a_in)
        avg_cross.append(a_cr)
        has_intra.append(n_in > 0)
        has_cross.append(n_cr > 0)
        print(f"{name:<10} {n_all:<10} {n_in:<6} {n_cr:<6} "
              f"{fmt(a_all):<12} {fmt(p_all):<12} "
              f"{fmt(a_in):<14} {fmt(a_cr):<14}")
    print("==========================================\n")

    color_map = {'DCQCN': '#1f77b4', 'HPCC': '#ff7f0e', 'TIMELY': '#2ca02c',
                 'FRP': '#17becf', 'ROCC': '#8c564b'}
    colors = [color_map.get(n, '#777777') for n in names]

    save_bar_chart("fct_avg.png", names, avg_all, colors,
                   "Average FCT (μs)", "Average FCT of Flows per Algorithm")
    save_bar_chart("fct_p99.png", names, p99_all, colors,
                   "p99 FCT (μs)", "p99 FCT of Flows per Algorithm")

    x = np.arange(len(names))
    width = 0.35
    fig, ax = plt.subplots(figsize=(12, 6))
    intra_vals = [v if h and not np.isnan(v) else 0 for v, h in zip(avg_intra, has_intra)]
    cross_vals = [v if h and not np.isnan(v) else 0 for v, h in zip(avg_cross, has_cross)]
    b1 = ax.bar(x - width / 2, intra_vals, width, label='intra-DC', color='steelblue')
    b2 = ax.bar(x + width / 2, cross_vals, width, label='cross-DC', color='orange')

    positive_values = [v for v in intra_vals + cross_vals if v > 0]
    if positive_values:
        ax.set_yscale('log')
        ax.set_ylim(bottom=min(positive_values) * 0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(names)
    ax.set_ylabel("Average FCT (μs, log)" if positive_values else "Average FCT (μs)")
    ax.set_title("Average FCT: Intra-DC vs Cross-DC Flows")
    ax.legend()

    for bar, val, has in list(zip(b1, avg_intra, has_intra)) + list(zip(b2, avg_cross, has_cross)):
        if has and not np.isnan(val):
            ax.text(bar.get_x() + bar.get_width() / 2, val,
                    f'{val:.1f}', ha='center', va='bottom', fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS_DIR, "fct_intra_cross.png"), dpi=150)
    plt.close(fig)

    print("已生成:")
    print(f"  {RESULTS_DIR}/fct_avg.png")
    print(f"  {RESULTS_DIR}/fct_p99.png")
    print(f"  {RESULTS_DIR}/fct_intra_cross.png")


if __name__ == "__main__":
    main()