#!/usr/bin/env python3
"""
单算法仿真脚本：运行指定算法并生成图像
用法: python3 run_single_algo.py <ccMode> <algo_name>
示例: python3 run_single_algo.py 3 HPCC
"""

import subprocess
import sys
import os
import re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

NS3_DIR = "/home/shemuping/newCode/ns3-FRP/simulator/ns-3.39"
DUMP_DIR = "/home/shemuping/newCode/ns3-FRP/dump" 
RESULTS_DIR = "/home/shemuping/newCode/ns3-FRP/results"
FCT_DIR = os.path.join(RESULTS_DIR, "fct")
PFC_DIR = os.path.join(RESULTS_DIR, "pfc")
CONFIG_FILE = "examples/PowerTCP/config.txt"  # 使用config.txt而不是config-burst.txt
DURATION = 0.150

def run_and_plot(ccMode, algo_name):
    """运行仿真并绘图"""
    
    print(f"\n{'='*80}")
    print(f"仿真: {algo_name} (ccMode={ccMode})")
    print(f"{'='*80}\n")
    
    # 1. 更新配置
    config_path = os.path.join(NS3_DIR, CONFIG_FILE)
    with open(config_path, 'r') as f:
        content = f.read()
    
    content = re.sub(r'SIMULATOR_STOP_TIME\s*[=\s]+[\d.]+', f'SIMULATOR_STOP_TIME {DURATION}', content)
    content = re.sub(r'CC_MODE\s+\d+', f'CC_MODE {ccMode}', content)
    
    # 启用队列监控
    if 'ENABLE_QLEN_MON' not in content:
        # 在QLEN_MON_START前添加
        content = re.sub(r'(QLEN_MON_START)', f'ENABLE_QLEN_MON 1\n\1', content)
    else:
        content = re.sub(r'ENABLE_QLEN_MON\s+\d+', 'ENABLE_QLEN_MON 1', content)
    
    # 设置正确的时间范围 (0到仿真结束)
    content = re.sub(r'QLEN_MON_START\s+\d+', 'QLEN_MON_START 0', content)
    content = re.sub(r'QLEN_MON_END\s+\d+', f'QLEN_MON_END {int(DURATION * 1e9)}', content)
    
    with open(config_path, 'w') as f:
        f.write(content)
    
    print(f"✓ 配置已更新")
    
    # 2. 编译
    print("编译中...")
    subprocess.run(f"cd {NS3_DIR} && ./ns3 build >/dev/null 2>&1", shell=True)
    print("✓ 编译完成\n")
    
    # 3. 运行仿真
    log_file = f"/tmp/algo_cc{ccMode}.log"
    print(f"运行仿真...")
    cmd = f"cd {NS3_DIR} && timeout 90 ./build/examples/PowerTCP/ns3.39-crossDC-evaluation-optimized --conf={CONFIG_FILE} --algorithm={ccMode} > {log_file} 2>&1"
    result = subprocess.run(cmd, shell=True, timeout=100)
    
    if result.returncode != 0:
        print(f"✗ 仿真失败")
        return
    
    log_size = os.path.getsize(log_file)
    print(f"✓ 仿真完成 ({log_size/1024:.1f}KB)\n")
    
    # 4. 解析日志
    print("解析日志...")
    with open(log_file, 'r') as f:
        lines = f.readlines()
    
    # 解析队列数据 - 监控 Switch 32（根据实际拓扑）
    MONITORED_SWITCHES = {32}
    HIGHLIGHT_SWITCHES = {32}
    # key=(sw_id, port) -> {times:[], qs:[]}
    queue_data = {}
    for l in lines:
        l = l.strip()
        # DCQCN/HPCC/TIMELY 使用 DCQCN_QLEN
        if '[DCQCN_QLEN]' in l and ccMode in [1, 3, 7]:
            parts = l.split()
            if len(parts) >= 5:
                sw = int(parts[2])
                port = int(parts[3])
                if sw in MONITORED_SWITCHES:
                    key = (sw, port)
                    if key not in queue_data:
                        queue_data[key] = {'times': [], 'qs': []}
                    queue_data[key]['times'].append(float(parts[1]) / 1e9 * 1000)  # timestep -> ms
                    queue_data[key]['qs'].append(float(parts[4]) / 1024.0)         # Bytes -> KB

        # FRP/ROCC 使用 FRP_DATA_SW
        if '[FRP_DATA_SW]' in l and ccMode in [13, 14]:
            parts = l.split()
            if len(parts) >= 8:
                sw = int(parts[2])
                port = int(parts[3])
                if sw in MONITORED_SWITCHES and int(parts[7]) == ccMode:
                    key = (sw, port)
                    if key not in queue_data:
                        queue_data[key] = {'times': [], 'qs': []}
                    queue_data[key]['times'].append(float(parts[1]) * 1000)  # s -> ms
                    queue_data[key]['qs'].append(float(parts[5]))             # KB
    
    # 解析接收侧 per-flow 速率 ([FLOW TP])
    flow_tp = {}  # key=(src,dst,sport,dport) → {times:[], rates:[]}
    for l in lines:
        l = l.strip()
        m = re.search(
            r'\[FLOW TP\] Src (\d+) Dst (\d+) pg (\d+) sport (\d+) dport (\d+) '
            r'throughput ([\d.e+\-]+) time ([\d.e+\-]+) m_recv_bytes (\d+)', l)
        if m:
            src = int(m.group(1))
            dst = int(m.group(2))
            sport = int(m.group(4))
            dport = int(m.group(5))
            tp_bps = float(m.group(6))
            t_sec = float(m.group(7))
            key = (src, dst, sport, dport)
            if key not in flow_tp:
                flow_tp[key] = {'times': [], 'rates': []}
            flow_tp[key]['times'].append(t_sec * 1000)  # 转换为 ms
            flow_tp[key]['rates'].append(tp_bps / 1e9)   # bps → Gbps
    
    # 时间轴：按时间周期采样，直接使用日志中的真实时间戳，不再用 linspace 兜底
    active_flows = sorted(flow_tp.keys())
    print(f"✓ 解析完成: 队列 {len(queue_data)} 个 (sw,port), 接收侧流 {len(active_flows)} 条")
    # print(f"  活跃主机列表: {active_hosts}")
    # for h in active_hosts:
    #     print(f"    Host {h}: {len(host_rates[h])} 个速率数据点")
    # for fk in active_flows:
    #     print(f"    Flow {fk}: {len(flow_tp[fk]['times'])} 个接收速率数据点")
    # print()

    # 4.5 解析 FCT 文件 (单位: ns-3 timestep, 默认 = ns)
    fct_ns_list = []  # 各流实际 FCT (纳秒)
    fct_standalone_list = []  # 独立传输 FCT (纳秒)
    fct_file_path = "/home/shemuping/newCode/ns3-FRP/results/fct/fct.txt"
    if os.path.exists(fct_file_path):
        with open(fct_file_path, 'r') as ff:
            for fl in ff:
                fl = fl.strip()
                if not fl:
                    continue
                # sip dip sport dport size startTime fct standalone_fct
                p = fl.split()
                if len(p) >= 8:
                    fct_ns_list.append(int(p[6]))
                    fct_standalone_list.append(int(p[7]))
    
    # 5. 绘图
    print("生成图像...")
    plt.rcParams['font.family'] = 'DejaVu Sans'
    plt.rcParams['axes.unicode_minus'] = False
    
    fig, axes = plt.subplots(2, 1, figsize=(18, 12), sharex=True)
    fig.suptitle(f'{algo_name} (ccMode={ccMode}) - Burst Traffic Analysis',
                 fontsize=18, fontweight='bold')
    
    colors = ['red', 'blue', 'green', 'orange', 'purple', 'cyan', 'lime', 'brown', 'pink', 'gray',
              'magenta', 'olive', 'teal', 'navy', 'maroon', 'coral', 'gold', 'indigo']
    
    # 子图1: 接收侧 per-flow Throughput ([FLOW TP])
    ax2 = axes[0]
    print(f"绘制 {len(active_flows)} 条流的接收速率...")
    for idx, fk in enumerate(active_flows[:15]):  # 最多显示15条流
        times = flow_tp[fk]['times']
        rates = flow_tp[fk]['rates']
        n = min(len(rates), len(times))
        if n == 0:
            continue
        times = times[:n]
        rates = rates[:n]
        c = colors[idx % len(colors)]
        label = f'{fk[0]}->{fk[1]} (sp={fk[2]})'
        ax2.plot(times, rates, color=c, lw=2, label=label, alpha=0.85)
        print(f"  {label}: {n} 点, 速率范围 [{min(rates):.2f}, {max(rates):.2f}] Gbps")

    ax2.set_ylabel('Receive Throughput (Gbps)', fontsize=14, fontweight='bold')
    ax2.set_title(f'RX Throughput - per-flow ({len(active_flows)} flows)', fontsize=15, fontweight='bold')
    ax2.legend(loc='upper right', fontsize=10, ncol=3, framealpha=0.9)
    ax2.grid(True, alpha=0.3, linestyle='--')
    ax2.set_ylim(0, 110)
    ax2.tick_params(labelsize=11)
    
    # 子图2: 队列长度
    ax3 = axes[1]
    queue_colors = {32: 'blue'}
    for (sw, port), qd in sorted(queue_data.items()):
        ts, qs = qd['times'], qd['qs']
        if not ts:
            continue
        c = queue_colors.get(sw, 'gray')
        is_highlight = sw in HIGHLIGHT_SWITCHES
        lw = 2.5 if is_highlight else 1.5
        ls = '-' if is_highlight else '--'
        alpha = 0.9 if is_highlight else 0.6
        label = f'Switch {sw} Port {port}' + (' (highlight)' if is_highlight else '')
        ax3.plot(ts, qs, color=c, lw=lw, ls=ls, alpha=alpha, label=label)
    
    ax3.axhline(y=300, color='blue', ls='--', lw=2, alpha=0.7, label='qRef = 300KB')
    ax3.axhline(y=300, color='red', ls=':', lw=1.5, alpha=0.6, label='q_th = 300KB')
    
    ax3.set_ylabel('Queue Length (KB)', fontsize=14, fontweight='bold')
    ax3.set_xlabel('Time (ms)', fontsize=14, fontweight='bold')
    ax3.set_title('Switch 32 Queue Length', fontsize=15, fontweight='bold')
    ax3.legend(loc='upper right', fontsize=10, framealpha=0.9, ncol=2)
    ax3.grid(True, alpha=0.3, linestyle='--')
    
    all_qs = [q for qd in queue_data.values() for q in qd['qs']]
    max_q = max(all_qs) if all_qs else 1000
    ax3.set_ylim(-50, max_q * 1.15)
    ax3.tick_params(labelsize=11)
    
    plt.tight_layout()
    
    output_file = os.path.join(RESULTS_DIR, f'burst_{algo_name.lower()}_cc{ccMode}.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"✓ 图像已保存: {output_file}\n")
    
    # 6. 打印统计
    print(f"{'='*80}")
    print(f"统计信息 - {algo_name}")
    print(f"{'='*80}")
    print(f"活跃流数量: {len(active_flows)}")
    print(f"监控交换机数量: {len(queue_data)} 个 (sw,port)")
    for (sw, port), qd in sorted(queue_data.items()):
        qs = qd['qs']
        marker = '★' if sw in HIGHLIGHT_SWITCHES else ' '
        print(f"  {marker} Switch {sw:>2} Port {port}: "
              f"max={max(qs):.0f} KB, "
              f"min={min(qs):.0f} KB, "
              f"mean={np.mean(qs):.0f} KB, "
              f"points={len(qs)}")

    # 7. 解析 FCT 文件 (仿照 run_single_workload_algo.py 的做法)
    fct_path = os.path.join(FCT_DIR, "fct.txt")
    fct_records = []
    if os.path.exists(fct_path):
        with open(fct_path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                parts = line.split()
                if len(parts) < 8:
                    continue
                try:
                    fct_records.append({
                        'sip':  int(parts[0], 16),     # hex
                        'dip':  int(parts[1], 16),     # hex
                        'sport': int(parts[2]),
                        'dport': int(parts[3]),
                        'size':  int(parts[4]),
                        'start_ns': int(parts[5]),
                        'fct_ns':   int(parts[6]),
                        'sfct_ns':  int(parts[7]),
                    })
                except ValueError:
                    continue

    print(f"\n--- FCT (Flow Completion Time) ---")
    if fct_records:
        print(f"完成流数: {len(fct_records)}")
        # Table header (fields separated by space for visual clarity)
        print(f"{'No.':<5}{'SIP':<13}{'DIP':<13}{'Sport':<8}{'Dport':<8}"
              f"{'Size(KB)':<11}{'Start(ms)':<11}{'FCT(ms)':<11}{'Standalone FCT(ms)':<19}{'Slowdown':<10}")
        print('-' * 108)
        for idx, fc in enumerate(fct_records, 1):
            size_kb = fc['size'] / 1024.0
            start_ms = fc['start_ns'] / 1e6
            fct_ms = fc['fct_ns'] / 1e6
            sfct_ms = fc['sfct_ns'] / 1e6
            slowdown = (fct_ms / sfct_ms) if sfct_ms > 0 else 0.0
            print(f"{idx:<5}0x{fc['sip']:08x}  0x{fc['dip']:08x}  "
                  f"{fc['sport']:<8}{fc['dport']:<8}{size_kb:<11.1f}"
                  f"{start_ms:<11.3f}{fct_ms:<11.3f}{sfct_ms:<19.3f}{slowdown:<10.2f}")

        fct_ms_arr = np.array([f['fct_ns']/1e6 for f in fct_records])
        sfct_ms_arr = np.array([f['sfct_ns']/1e6 for f in fct_records])
        slowdown = fct_ms_arr / sfct_ms_arr
        print(f"\n  FCT 统计 (ms):")
        print(f"    avg={np.mean(fct_ms_arr):.3f}, min={np.min(fct_ms_arr):.3f}, "
              f"max={np.max(fct_ms_arr):.3f}, "
              f"p50={np.percentile(fct_ms_arr,50):.3f}, p95={np.percentile(fct_ms_arr,95):.3f}, "
              f"p99={np.percentile(fct_ms_arr,99):.3f}")
        print(f"  Slowdown (FCT / standalone FCT):")
        print(f"    avg={np.mean(slowdown):.3f}x, min={np.min(slowdown):.3f}x, "
              f"max={np.max(slowdown):.3f}x")
    else:
        print("FCT 数据: 无 (检查 /home/shemuping/newCode/ns3-FRP/results/fct/fct.txt)")

    print(f"\n图像文件: {output_file}")
    print(f"{'='*80}\n")
    
    return output_file

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("用法: python3 run_single_algo.py <ccMode> <algo_name>")
        print("示例: python3 run_single_algo.py 3 HPCC")
        print("\n支持的算法:")
        print("  1  - DCQCN")
        print("  3  - HPCC")
        print("  7  - TIMELY")
        print("  13 - FRP (ours)")
        print("  14 - ROCC")
        sys.exit(1)
    
    ccMode = int(sys.argv[1])
    algo_name = sys.argv[2]
    
    run_and_plot(ccMode, algo_name)
