# LonghaulCC

## 1. 程序简介

本目录包含基于 ns-3.39 的跨数据中心 RDMA 长距离拥塞控制实验。主程序是
`longhaul-convergence.cc`，目前用于运行和比较以下算法：

- DCQCN：`dcqcn`
- HPCC：`hpcc`
- TIMELY：`timely`

程序读取拓扑、公共配置和流量场景文件，创建 Qbb/RDMA 网络，配置路由和流，
最后将吞吐率、FCT、DCI 链路等结果写入结果目录。

所有命令默认在 ns-3.39 根目录执行：

```bash
cd /home/shemuping/newCode/ns3-FRP/simulator/ns-3.39
```

## 2. 网络拓扑

拓扑文件为：

```text
examples/LonghaulCC/topology-longhaul.txt
```

当前拓扑已经缩小为每个数据中心 32 个 host。

![LonghaulCC 两个跨域数据中心网络拓扑](topology-longhaul.png)

节点编号如下：

| 设备 | DC0 | DC1 |
|---|---:|---:|
| Host | 0--31 | 41--72 |
| Leaf | 32--35 | 73--76 |
| Spine | 36--39 | 77--80 |
| Gateway | 40 | 81 |

拓扑特征：

- 每个 DC 有 32 个 host、4 个 leaf、4 个 spine 和 1 个 gateway；
- 每个 leaf 连接 8 个 host；
- leaf 与 spine 采用全互联；
- 4 个 spine 均连接到本 DC 的 gateway；
- 两个 gateway 通过 DCI 互联，带宽为 200 Gbps，单向传播时延为 5 ms；
- DC 内链路带宽为 100 Gbps，单向传播时延为 1.5 us；
- 总规模为 82 个节点、64 个 host、18 个交换机和 105 条链路。

流量场景文件为 `flow-longhaul-s0.txt` 到 `flow-longhaul-s5.txt`。每行第 5 列是
以字节为单位的流大小；当前大流使用 3,000,000,000 B。流量文件中的 host 编号必须与拓扑文件中的编号一致。

## 3. 编译和检查拓扑

编译主程序：
s
```bash
./ns3 build longhaul-convergence -j2
```

检查拓扑节点数量、链路、DCI 和连通性：

```bash
python3 examples/LonghaulCC/validate_longhaul_topology.py
```

## 4. 运行实验

### 4.1 快速冒烟测试

下面的命令只运行场景 `s0`，每种算法运行一次，仿真到 0.03 秒，适合检查程序是否
能够启动并生成结果：

```bash
python3 examples/LonghaulCC/run-longhaul-baseline.py \
  --config examples/LonghaulCC/config-longhaul-common.txt \
  --flow-files examples/LonghaulCC/flow-longhaul-s0.txt \
  --algorithms dcqcn hpcc timely \
  --runs 1 \
  --stop-times 0.03 \
  --skip-build \
  --output-root /tmp/longhaul-32h-smoke
```

`--skip-build` 表示复用已有编译结果；如果尚未编译主程序，应删除该选项。
`--stop-times 0.03` 主要用于启动检查，不代表完整的收敛实验时长。

### 4.2 默认实验

```bash
python3 examples/LonghaulCC/run-longhaul-baseline.py
```

默认使用完整配置文件中的 `s0` flow，比较 DCQCN、HPCC、TIMELY，每个 flow 运行 1
次，结果默认写入：

```text
results/longhaul/
```

### 4.3 指定场景、算法和重复次数

```bash
python3 examples/LonghaulCC/run-longhaul-baseline.py \
  --config examples/LonghaulCC/config-longhaul-common.txt \
  --flow-files examples/LonghaulCC/flow-longhaul-s2.txt \
  --stop-times 1.50 \
  --algorithms dcqcn hpcc timely \
  --runs 3 \
  --output-root results/longhaul
```

### 4.4 运行完整场景集合

```bash
python3 examples/LonghaulCC/run-longhaul-baseline.py \
  --flow-files examples/LonghaulCC/flow-longhaul-s0.txt examples/LonghaulCC/flow-longhaul-s1.txt \
              examples/LonghaulCC/flow-longhaul-s2.txt examples/LonghaulCC/flow-longhaul-s3.txt \
              examples/LonghaulCC/flow-longhaul-s4.txt examples/LonghaulCC/flow-longhaul-s5.txt \
  --stop-times 0.38 1.50 1.50 0.60 1.50 1.50 \
  --runs 5
```

该命令通过命令行显式选择 `s0` 到 `s5` 的 flow 和停止时间，耗时会明显增加。

### 4.5 直接运行 ns-3 主程序

如需绕过批量运行脚本，也可以直接运行主程序：

```bash
./ns3 run "longhaul-convergence \
  --conf=examples/LonghaulCC/config-longhaul-common.txt \
  --flow-file=examples/LonghaulCC/flow-longhaul-s0.txt \
  --stop-time=0.38 \
  --cc=dcqcn \
  --seed=1 \
  --run=1"
```

基准算法使用 `--cc=dcqcn|hpcc|timely`；可通过 `--cc=frp` 单独运行 FRP。批量基准实验建议使用
`run-longhaul-baseline.py`，因为它会自动保存配置快照、标准输出和运行元数据。

参数优先级固定为：C++ 默认值 < 配置文件 < 显式命令行参数。runner 不改写原始配置文件，
而是为每次运行生成包含输出路径的有效 `config.txt`，并选择 flow、停止时间、算法、重复次数
和输出目录；场景名从 flow 文件名生成。
包大小、速率、窗口、buffer、采样周期和 ECN map 等普通参数不再注册为命令行选项，
统一放在 `config-longhaul-common.txt` 中维护。

## 5. 结果分析

分析结果：

```bash
python3 examples/LonghaulCC/analyze-longhaul.py \
  --root results/longhaul
```

绘制曲线：

```bash
python3 examples/LonghaulCC/plot-longhaul.py \
  --root results/longhaul \
  --all-scenarios
```

单次运行结果通常位于：

```text
results/longhaul/<scenario>/<algorithm>/seed<seed>-run<run>/
```

其中包括运行元数据、per-flow 发送 payload/wire 速率、接收 goodput、双向 DCI TX/利用率、
measured RTT、FCT 和 PFC 等文件。

## 6. 注意事项

S0–S5 中原有的 1 TB 大流已调整为 3,000,000,000 B，低于当前 RDMA 数据序号的
32 位上限。`sender-rate.csv` 的 payload TX 计入重传，接收 goodput 只计入按序有效 payload；
无丢包场景完成后两者都应覆盖 flow size，有重传时 TX payload 可更大。FCT 参照值使用该 flow
路径的 RTT 和实际 PPP packet header 序列化字节。运行大流场景时应使用上方对应的停止时间，
并检查每条流均有完成记录以及接收 payload 与 flow size 闭合。

## Proposed：接收侧预测与近源 CNP

核心实现位于 `longhaul-research.h`，由 `longhaul-convergence.cc` 包含并调用
`ResearchSetup()`。使用 `--cc=proposed` 启用报告版本：接收侧周期 200 µs、
近源周期 50 µs、队列参考值 250000 B、预测权重 0.5，并开启旧遥测回退、
目标上升限幅、减流时虚拟积压限制。RNIC 使用 DCQCN 执行真实 CNP；跨域
遥测与目标反馈仍由延迟事件模拟。

在仓库根目录执行：

```bash
./simulator/ns-3.39/ns3 build longhaul-convergence -j2
python3 simulator/ns-3.39/examples/LonghaulCC/run-research.py \
  --variants proposed dcqcn --scenarios finite --stop 0.08 \
  --wan-delay-us 1000 --no-window --jobs 1 \
  --output results/longhaul-proposed-new
```

该脚本为同一个 `longhaul-convergence` 程序生成小规模拓扑和 flow 文件，并通过命令行
传入这些输入：4 个发送端、每流 100 MB、10 ms 开始；主机链路 100 Gbps，跨域链路
200 Gbps，接收出口 100 Gbps。输出目录使用新路径，避免覆盖已完成实验。

各算法目录中，`bottleneck.csv` 保存实际队列、预测队列、虚拟队列、遥测速率、
目标速率及累计 CNP；`sender-rate.csv`、`receiver-goodput.csv`、`fct.csv`
保存流量指标，`metadata.json` 记录算法及控制参数。Proposed 的算法字段为
`proposed`，底层 `cc_mode` 为 1。

主程序只接受六个命令行入口；research receiver、控制周期、DCI 节点和输出路径等
参数放在 `--conf` 指定的配置文件中。`run-research.py` 会为每个运行目录生成
完整的 `config.txt`，并通过 `--cc`、`--flow-file`、`--stop-time` 选择运行，避免
把 research 参数重复注册为 C++ 命令行选项。

当前原型要求发送主机直接连接源侧 DCI、目标接收主机直接连接接收侧 DCI，
且所有流发往同一接收主机。上面的 runner 生成满足条件的拓扑；目录内默认的
多层 `topology-longhaul.txt` 不满足这一限制，不能只追加 `--cc=proposed`
就用于该拓扑。多跳近源 CNP 路由和瓶颈选择尚未扩展。

`reactive-cnp` / `predictive-cnp` 仍由研究 runner 写入独立的 `config.txt`，其中
明确设置 `RESEARCH_CONTROL`，不会隐式改变基线配置。
