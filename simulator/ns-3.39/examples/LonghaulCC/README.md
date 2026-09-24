# LonghaulCC

## 1. 程序简介

本目录包含基于 ns-3.39 的跨数据中心 RDMA 长距离拥塞控制实验。主程序是
`longhaul-convergence.cc`，目前用于运行和比较以下算法：

- DCQCN：`dcqcn`
- HPCC：`hpcc`
- TIMELY：`timely`
- Bifrost：`bifrost`
- FRP：`frp`
- RoCC：`rocc`
- Proposed R2：`proposed`

RoCC 在 40/100 Gbps 链路上使用论文参数。论文未给出 200 Gbps 参数；当前实现按
带宽比例放大 `Qref/Qmid/Qmax`，并保留 100 Gbps 的 PI 基础增益，该策略会写入运行元数据。

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

当前拓扑每个数据中心有 32 个 host。

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

例如 `0 → Leaf32 → Spine36 → DCI40 → DCI81 → Spine77 → Leaf73 → 41` 是一条跨数据中心路径。

流量场景文件为 `flow-longhaul-s0.txt` 到 `flow-longhaul-s5.txt`。每行第 5 列是
以字节为单位的流大小；当前大流使用 3,000,000,000 B。流量文件中的 host 编号必须与拓扑文件中的编号一致。

## 3. 编译

编译主程序：

```bash
./ns3 build longhaul-convergence -j2
```

当前只保留 `longhaul-convergence` 仿真目标，独立的 Proposed 测试目标已删除。

## 4. 运行实验

### 4.1 快速冒烟测试

下面的命令只运行场景 `s0` 的 DCQCN，仿真到 0.03 秒，适合检查程序是否
能够启动并生成结果：

```bash
python3 examples/LonghaulCC/run-longhaul.py \
  --algorithm dcqcn \
  --config examples/LonghaulCC/config-longhaul.txt \
  --flow-files examples/LonghaulCC/flow-longhaul-s0.txt \
  --runs 1 \
  --stop-times 0.03 \
  --skip-build \
  --output-root /tmp/longhaul-s0-smoke
```

`--skip-build` 表示复用已有编译结果；如果尚未编译主程序，应删除该选项。
`--stop-times 0.03` 只检查启动和输出，不代表流完成或收敛。再次运行时应换一个
`--output-root`，因为 runner 不覆盖已有运行目录。

### 4.2 默认实验

```bash
python3 examples/LonghaulCC/run-longhaul-all.py
```

默认使用公共配置中的 S0 流文件和 0.38 s 停止时间，依次运行 DCQCN、HPCC、TIMELY、
Bifrost、FRP、RoCC、Proposed，每种算法运行一次。结果写到仓库根目录的：

```text
results/longhaul/
```

### 4.3 指定场景、算法和重复次数

```bash
python3 examples/LonghaulCC/run-longhaul.py \
  --algorithm hpcc \
  --config examples/LonghaulCC/config-longhaul.txt \
  --flow-files examples/LonghaulCC/flow-longhaul-s2.txt \
  --stop-times 1.50 \
  --runs 3 \
  --output-root ../../results/longhaul-hpcc-s2
```

### 4.4 运行完整场景集合

```bash
python3 examples/LonghaulCC/run-longhaul-all.py \
  --flow-files examples/LonghaulCC/flow-longhaul-s0.txt examples/LonghaulCC/flow-longhaul-s1.txt \
              examples/LonghaulCC/flow-longhaul-s2.txt examples/LonghaulCC/flow-longhaul-s3.txt \
              examples/LonghaulCC/flow-longhaul-s4.txt examples/LonghaulCC/flow-longhaul-s5.txt \
  --stop-times 0.38 1.50 1.50 0.60 1.50 1.50 \
  --runs 5 \
  --output-root ../../results/longhaul-s0-s5
```

该命令显式选择 S0–S5 的流文件和停止时间，共执行 6 场景 × 7 算法 × 5 次运行。

指定算法使用 `run-longhaul.py --algorithm <名称>`；全部算法使用 `run-longhaul-all.py`。
两者都会保存配置快照、标准输出和运行元数据；已存在的数据目录不会被覆盖。

主程序的 CLI 只包含 `--conf`、`--cc`、`--flow-file`、`--stop-time`、`--seed`、`--run`。
参数优先级为：C++ 默认值 < 配置文件 < 显式命令行参数。runner 不改写原始配置文件，
而是为每次运行生成包含输出路径的有效 `config.txt`；场景名从流文件名生成。
包大小、速率、窗口、buffer、采样周期和 ECN map 等普通参数不再注册为命令行选项，
统一放在 `config-longhaul.txt` 中维护。

## 5. 结果分析

一次生成 `summary.csv`、`run-summary.csv` 和 `plots/` 下的图：

```bash
python3 examples/LonghaulCC/analyze-longhaul.py \
  --root ../../results/longhaul
```

这里的 `../../results/longhaul` 与默认 runner 的输出目录相同。分析只读取运行元数据中
`status=ok` 的目录，生成每流阶段的 `summary.csv`、每次运行的 `run-summary.csv` 和
`plots/` 下的速率、收敛、DCI 队列/利用率、公平性、RTT 等图；失败运行的日志仍保留。
若运行时指定了其他 `--output-root`，分析时应将 `--root` 设为同一目录。

单次运行结果通常位于：

```text
../../results/longhaul/<scenario>/<algorithm>/seedconfig-run<run>/
```

未显式指定 `--seed` 时，目录名使用 `seedconfig`；显式指定时使用 `seed<值>`。
每个目录保存 `config.snapshot.txt`、有效 `config.txt`、`runner-metadata.json`、
`metadata.json`、`stdout.log`，以及发送速率、接收 goodput、DCI、RTT、FCT、PFC CSV。

## 6. 注意事项

S0–S5 中原有的 1 TB 大流已调整为 3,000,000,000 B，低于当前 RDMA 数据序号的
32 位上限。`sender-rate.csv` 的 payload TX 计入重传，接收 goodput 只计入按序有效 payload；
无丢包场景完成后两者都应覆盖 flow size，有重传时 TX payload 可更大。FCT 参照值使用该 flow
路径的 RTT 和实际 PPP packet header 序列化字节。运行大流场景时应使用上方对应的停止时间，
并检查每条流均有完成记录以及接收 payload 与 flow size 闭合。

## Proposed R2（2026-09-24）

`--cc=proposed` 仅运行 R2。核心位于 `longhaul-proposed.h`：接收端周期 STATE、
源端固定分箱发送历史、单一速率公式和逐包 FIFO 整形。旧 Proposed/R1 实现、
旧算法入口和旧配置键已删除；历史设计文档保留。

源端使用 `u=min(F,Csource,max(0,Ceff-x-max(0,qhat-qref)/tau))`，不叠加恢复斜坡、
危险状态控制或虚拟队列。RNIC 仍为 DCQCN。主程序仍只有原六项 CLI，控制参数
统一放在 `PROPOSED_*` 配置项中；旧 `R1_*` / `RESEARCH_*` 会明确报未知配置键。

三项消融使用同一整形/CNP：`proposed-static` 静态上限，`proposed-reactive` 旧快照
反应，`proposed-extrapolate` 接收测得速率的常值外推。没有重新保留旧控制器。
这些消融通过配置项 `PROPOSED_PREDICTOR` 选择，不是独立的 `--cc` 算法。

当前源码会沿流的路径识别 DCI 和接收主机的出口，在多跳路径上建立控制组。
观测点为接收 Leaf 的主机出口；STATE 和 CNP 经过真实多跳路由。多接收主机独立分组，
双向 WAN 独立分配速率，各组共用原物理 PG/MMU。S0–S5 缩小流量回归均完成，含 S4
的16条双向流；S4出现公共 headroom 不足导致的丢包，完整规模性能尚未验收。

默认情况下，Proposed 控制日志写入该运行目录的 `metadata.json.control.csv`；
同名前缀的 `.receiver.csv`、`.events.csv`、`.packets.csv`、`.receiver-events.csv`
分别保存接收统计、STATE/CNP/目标更新及离线逐包数据。这些逐包 CSV 不进入在线控制器。
`metadata.json` 的 `proposed_parameters.version=2`，其 `groups` 数组包含各组位置、有效时延和历史存储大小。
其余发送率、goodput、RTT、FCT、PFC 日志保留。

多跳适配、边界及验证记录见 [R2 实现说明](PROPOSED_R2_IMPLEMENTATION.md)。
