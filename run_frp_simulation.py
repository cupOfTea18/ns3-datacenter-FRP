#!/usr/bin/env python3
"""
FRP仿真一键运行与可视化脚本
用法: python3 run_and_plot.py [--algorithm ALG] [--duration MS] [--conf CONF]
"""

import argparse
import subprocess
import sys
import os
import re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# 配置
NS3_DIR = "/home/shemuping/newCode/ns3-FRP/simulator/ns-3.39"
RESULTS_DIR = "/home/shemuping/newCode/ns3-FRP/results"
DEFAULT_CONF = "examples/PowerTCP/config.txt"
DEFAULT_ALGORITHM = 13
DEFAULT_DURATION = 0.020  # 20ms

def run_simulation(algorithm=DEFAULT_ALGORITHM, conf=DEFAULT_CONF, duration=DEFAULT_DURATION):
    """运行ns-3仿真"""
    algo_name = "ROCC" if algorithm == 14 else "FRP"
    print("=" * 60)
    print(f"Step 1: 运行NS-3仿真 ({algo_name}, ccMode={algorithm})")
    print("=" * 60)
    
    # 更新仿真时长
    config_path = os.path.join(NS3_DIR, conf)
    if os.path.exists(config_path):
        with open(config_path, 'r') as f:
            content = f.read()
        # 更新SIMULATOR_STOP_TIME
        content = re.sub(
            r'SIMULATOR_STOP_TIME\s*=\s*[\d.]+',
            f'SIMULATOR_STOP_TIME={duration}',
            content
        )
        with open(config_path, 'w') as f:
            f.write(content)
        print(f"✓ 更新仿真时长: {duration}s")
    
    # 构建
    print("编译中...")
    build_cmd = f"cd {NS3_DIR} && ./ns3 build 2>&1 | tail -3"
    result = subprocess.run(build_cmd, shell=True, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"✗ 编译失败: {result.stderr}")
        sys.exit(1)
    print("✓ 编译完成")
    
    # 运行仿真
    log_file = "/tmp/frp_simulation.log"
    cmd = f"cd {NS3_DIR} && timeout 180 ./build/examples/PowerTCP/ns3.39-crossDC-evaluation-optimized --conf={conf} --algorithm={algorithm} > {log_file} 2>&1"
    print(f"运行仿真 (algorithm={algorithm}, {algo_name})...")
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=200)
    
    if result.returncode != 0:
        print(f"✗ 仿真失败: {result.stderr}")
        print(f"日志内容 (前100行):")
        with open(log_file, 'r') as f:
            for i, line in enumerate(f):
                if i >= 100:
                    break
                print(line.rstrip())
        sys.exit(1)
    
    print(f"✓ 仿真完成，日志: {log_file}")
    
    # 统计日志大小和行数
    log_size = os.path.getsize(log_file)
    with open(log_file, 'r') as f:
        log_lines = sum(1 for _ in f)
    print(f"  日志大小: {log_size / 1024:.1f}KB, {log_lines} 行")
    return log_file

def parse_log(log_file):
    """解析仿真日志"""
    print("\n" + "=" * 60)
    print("Step 2: 解析仿真日志")
    print("=" * 60)
    
    with open(log_file, 'r') as f:
        content = f.read()
    lines = content.split('\n')
    
    # 解析交换机数据
    sw13_time, sw13_F, sw13_q, sw13_type = [], [], [], []
    frp_count = 0
    rocc_count = 0
    for l in lines:
        l = l.strip()
        if '[FRP_DATA_SW]' in l:
            parts = l.split()
            if len(parts) >= 8 and int(parts[2]) == 13 and int(parts[3]) == 1:
                sw13_time.append(float(parts[1]) * 1e6)  # us
                sw13_F.append(float(parts[4]) / 1000)     # Mbps -> Gbps
                sw13_q.append(float(parts[5]))             # KB
                sw13_type.append(int(parts[7]))            # type (ccMode: 13=FRP, 14=ROCC)
                if int(parts[7]) == 13:
                    frp_count += 1
                elif int(parts[7]) == 14:
                    rocc_count += 1
    
    if frp_count > 0 or rocc_count > 0:
        print(f"  Sw13 type=0(FRP): {frp_count} 条, type=1(ROCC): {rocc_count} 条")
    
    # 解析主机速率
    rates = {}
    for i in range(8):
        rates[i] = {'time': [], 'rate': []}
    
    rate_line_idx = 0
    total_rate_lines = sum(1 for l in lines if '[FRP RATE]' in l)
    time_per_rate_line = 20.0 / total_rate_lines if total_rate_lines > 0 else 0
    
    for l in lines:
        l = l.strip()
        if '[FRP RATE]' in l:
            host_match = re.search(r'Host (\d+)', l)
            if host_match:
                host = int(host_match.group(1))
                if host in rates:
                    t = rate_line_idx * time_per_rate_line
                    rate_match = re.search(r'curRate=([\d.]+)G', l)
                    if rate_match:
                        r = float(rate_match.group(1))  # 已经是Gbps
                        rates[host]['time'].append(t)
                        rates[host]['rate'].append(r)
            rate_line_idx += 1
    
    # 解析ROCC规则触发日志
    rocc_rule1_count = sum(1 for l in lines if '[ROCC-RULE1]' in l)
    rocc_rule2_count = sum(1 for l in lines if '[ROCC-RULE2]' in l)
    rocc_no_action_count = sum(1 for l in lines if '[ROCC-NO-ACTION]' in l)
    if rocc_rule1_count > 0 or rocc_rule2_count > 0:
        print(f"  ROCC规则触发: RULE1={rocc_rule1_count}, RULE2={rocc_rule2_count}, NO-ACTION={rocc_no_action_count}")
    
    print(f"✓ 解析完成: Sw13 {len(sw13_F)} 点, 主机数据 {total_rate_lines} 行")
    return sw13_time, sw13_F, sw13_q, sw13_type, rates

def analyze_and_plot(sw13_time, sw13_F, sw13_q, sw13_type, rates, algorithm=13, output_file=None):
    """分析数据并绘图"""
    print("\n" + "=" * 60)
    print("Step 3: 分析与绘图")
    print("=" * 60)
    
    algo_name = "ROCC" if algorithm == 14 else "FRP"
    sw13_t_ms = np.array(sw13_time) / 1000.0
    
    # 统计数据
    print(f"\nSw13 Fair Rate: {min(sw13_F):.2f}~{max(sw13_F):.2f}G")
    print(f"Queue Length: {min(sw13_q):.0f}~{max(sw13_q):.0f}KB")
    
    # 找出活跃的主机
    active_hosts = []
    flow_completion = {}
    for h in sorted(rates.keys()):
        if rates[h]['time']:
            active_hosts.append(h)
            if h != 0:  # Host0通常是长流
                completion_time = rates[h]['time'][-1]
                flow_completion[h] = completion_time
                print(f"Host{h}: {len(rates[h]['time'])} 数据点, 完成时间 {completion_time:.3f}ms")
    
    # 最后50步统计
    if len(sw13_F) >= 50:
        F_last50 = sw13_F[-50:]
        q_last50 = sw13_q[-50:]
        print(f"\n最后50步 Fair Rate: 均值={np.mean(F_last50):.2f}G, 标准差={np.std(F_last50):.2f}G")
        print(f"最后50步 Queue: 均值={np.mean(q_last50):.0f}KB, 标准差={np.std(q_last50):.0f}KB")
    
    # 绘图
    plt.rcParams['font.family'] = 'DejaVu Sans'
    plt.rcParams['axes.unicode_minus'] = False
    
    fig, axes = plt.subplots(3, 1, figsize=(20, 16), sharex=True)
    
    # 标题
    hosts_str = ", ".join([f"Host{h}" for h in active_hosts])
    if algorithm == 14:
        title = f'ROCC Analysis (ccMode=14): {hosts_str}\n(Emergency Rules + FRP Formula beta=0.5, qRef=500KB, q_th=300KB)'
    else:
        title = f'FRP Analysis (ccMode=13): {hosts_str}\n(alpha=0.1, beta=0, sf=20, qRef=500KB, q_th=300KB)'
    fig.suptitle(title, fontsize=15, fontweight='bold')
    
    # 颜色映射
    colors = ['b', 'r', 'orange', 'purple', 'cyan', 'lime', 'yellow', 'pink']
    
    # Subplot 1: 发送速率
    ax1 = axes[0]
    for idx, h in enumerate(active_hosts):
        if rates[h]['time']:
            color = colors[idx % len(colors)]
            label = f'Host {h}'
            if h == 0:
                label += ' (WAN)'
            ax1.plot(rates[h]['time'], rates[h]['rate'], color=color, lw=1.5, 
                    label=label, alpha=0.9)
    
    # 标记流完成时间
    for h, t in flow_completion.items():
        idx = active_hosts.index(h) if h in active_hosts else 0
        color = colors[idx % len(colors)]
        ax1.axvline(x=t, color=color, ls=':', lw=1.5, alpha=0.6)
        ax1.text(t+0.2, 90-idx*10, f'Host{h}\n{t:.2f}ms', 
                fontsize=9, color=color, fontweight='bold')
    
    fair_share = 95.0 / len(active_hosts) if active_hosts else 23.75
    ax1.axhline(y=fair_share, color='green', ls='--', lw=1.5, alpha=0.5, 
               label=f'Fair share = {fair_share:.2f}G')
    ax1.set_ylabel('Sending Rate (Gbps)', fontsize=13)
    ax1.set_ylim(-2, 105)
    ax1.legend(loc='upper right', fontsize=11)
    ax1.set_title('1. Host Sending Rates', fontsize=14, fontweight='bold')
    ax1.grid(True, alpha=0.3)
    
    # Subplot 2: Fair Rate F
    ax2 = axes[1]
    ax2.plot(sw13_t_ms, sw13_F, 'g-', lw=2.5, label='Fair Rate F')
    
    for h, t in flow_completion.items():
        idx = active_hosts.index(h) if h in active_hosts else 0
        color = colors[idx % len(colors)]
        ax2.axvline(x=t, color=color, ls='--', lw=1.5, alpha=0.6)
    
    ax2.axhline(y=fair_share, color='gray', ls='--', lw=1.5, alpha=0.5, 
               label=f'Target: {fair_share:.2f}G')
    ax2.set_ylabel('Fair Rate F (Gbps)', fontsize=13)
    ax2.set_ylim(15, 100)
    ax2.legend(loc='upper right', fontsize=11)
    ax2.set_title('2. Fair Rate F', fontsize=14, fontweight='bold')
    ax2.grid(True, alpha=0.3)
    
    # Subplot 3: Queue Length
    ax3 = axes[2]
    ax3.fill_between(sw13_t_ms, 0, sw13_q, alpha=0.3, color='purple')
    ax3.plot(sw13_t_ms, sw13_q, 'm-', lw=1.5, label='Queue length')
    ax3.axhline(y=300, color='blue', ls='--', lw=1.5, alpha=0.6, label='qRef = 300KB')
    ax3.axhline(y=300, color='red', ls=':', lw=1, alpha=0.5, label='q_th = 300KB')
    
    for h, t in flow_completion.items():
        idx = active_hosts.index(h) if h in active_hosts else 0
        color = colors[idx % len(colors)]
        ax3.axvline(x=t, color=color, ls=':', lw=1, alpha=0.5)
    
    ax3.set_ylabel('Queue Length (KB)', fontsize=13)
    ax3.set_xlabel('Time (ms)', fontsize=13)
    ax3.set_ylim(-20, 1800)
    ax3.legend(loc='upper right', fontsize=11)
    ax3.set_title('3. Queue Length at Switch 13 (port 1)', fontsize=14, fontweight='bold')
    ax3.grid(True, alpha=0.3)
    
    plt.tight_layout()
    
    if output_file is None:
        output_file = os.path.join(RESULTS_DIR, 'frp_analysis.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"\n✓ 图像已保存: {output_file}")
    
    return output_file

def main():
    parser = argparse.ArgumentParser(description='FRP仿真一键运行与可视化')
    parser.add_argument('--algorithm', type=int, default=DEFAULT_ALGORITHM,
                       help=f'拥塞控制算法 (默认: {DEFAULT_ALGORITHM})')
    parser.add_argument('--duration', type=float, default=DEFAULT_DURATION,
                       help=f'仿真时长/秒 (默认: {DEFAULT_DURATION})')
    parser.add_argument('--conf', type=str, default=DEFAULT_CONF,
                       help=f'配置文件路径 (默认: {DEFAULT_CONF})')
    parser.add_argument('--output', type=str, default=None,
                       help='输出图像路径')
    
    args = parser.parse_args()
    
    try:
        # Step 1: 运行仿真
        log_file = run_simulation(
            algorithm=args.algorithm,
            conf=args.conf,
            duration=args.duration
        )
        
        # Step 2: 解析日志
        sw13_time, sw13_F, sw13_q, sw13_type, rates = parse_log(log_file)
        
        # Step 3: 分析与绘图
        output_file = analyze_and_plot(
            sw13_time, sw13_F, sw13_q, sw13_type, rates,
            algorithm=args.algorithm,
            output_file=args.output
        )
        
        print("\n" + "=" * 60)
        print("✓ 全部完成！")
        print("=" * 60)
        
    except Exception as e:
        print(f"\n✗ 错误: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == '__main__':
    main()
