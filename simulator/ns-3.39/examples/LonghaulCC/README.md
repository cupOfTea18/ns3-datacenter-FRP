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
- Proposed R3：`proposed`

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
Bifrost、Proposed，每种算法运行一次。结果写到仓库根目录的：

```text
results/longhaul-260924-1520/
```

目录名使用启动时的本地时间，格式为 `longhaul-YYMMDD-HHMM`；同一分钟内再次运行
会依次加上 `-2`、`-3`。runner 启动时会打印实际输出目录。显式传入
`--output-root` 时使用指定目录，已有运行数据仍不会被覆盖。

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

该命令显式选择 S0–S5 的流文件和停止时间，共执行 6 场景 × 5 算法 × 5 次运行。

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
  --root ../../results/longhaul-260924-1520
```

将示例时间戳替换为 runner 打印的实际目录。分析只读取运行元数据中
`status=ok` 的目录，生成每流阶段的 `summary.csv`、每次运行的 `run-summary.csv` 和
`plots/` 下的速率、收敛、DCI 队列/利用率、公平性、RTT 等图；失败运行的日志仍保留。
若运行时指定了 `--output-root`，分析时应将 `--root` 设为同一目录。

单次运行结果通常位于：

```text
../../results/longhaul-YYMMDD-HHMM/<scenario>/<algorithm>/seedconfig-run<run>/
```

未显式指定 `--seed` 时，目录名使用 `seedconfig`；显式指定时使用 `seed<值>`。
每个目录保存有效 `config.txt`、`runner-metadata.json`、
`metadata.json`、`stdout.log`，以及发送速率、接收 goodput、DCI、RTT、FCT、PFC CSV。

## 6. 注意事项

S0–S5 中原有的 1 TB 大流已调整为 3,000,000,000 B，低于当前 RDMA 数据序号的
32 位上限。`sender-rate.csv` 的 payload TX 计入重传，接收 goodput 只计入按序有效 payload；
无丢包场景完成后两者都应覆盖 flow size，有重传时 TX payload 可更大。FCT 参照值使用该 flow
路径的 RTT 和实际 PPP packet header 序列化字节。运行大流场景时应使用上方对应的停止时间，
并检查每条流均有完成记录以及接收 payload 与 flow size 闭合。

## Proposed R3（2026-09-29）

`--cc=proposed` 运行 R3，RNIC 保留 DCQCN。两个 DCI 创建为 `DciGatewayNode`，
普通 Leaf/Spine 仍为 `SwitchNode`。网关复用交换机转发、ECN、MMU 和 PFC；
专属逐流状态、CNP 反应、预算分配、消息与预测放在 `dci-gateway-node.{h,cc}`。
`longhaul-r3.h` 只负责路径验证、连接注册、配置及实验输出接线。

B 按实际向内出口和 PG 分组，合并同一反应窗口内的逐流 CNP，以有界加性方式恢复，
对同一物理端口的各组联合分配预算。B 反馈自己的跨域队列及逐流预算。
A 用 WAN 出口实际出队字节分箱重建远端积压，仅在 A 执行排空扣减，并分配逐流限额。
两端均使用共享有限缓存内的逐流逻辑队列；限速流不会阻塞其他可发送流，物理 PG 暂停仍有效。

当前 RNIC 主要通过 ACK/NACK 的 CNP 标志反馈；网关同时识别该标志和独立 CNP，
原报文继续转发。未隔离上游 ECN，因此这些事件属于**路径拥塞反馈**，不代表精确定位 B 内部瓶颈。
快照和需求消息经过真实路由，超过 24 条流记录时分片，接收端校验序号和注册代次后完整应用。

默认 runner 将 R3 输出前缀隔离为每次运行目录下的 `r3`：

- `r3.gateway-<node>.csv`：逐流队列、预算、实际字节、预测值、反馈及拒绝计数。
- `r3.gateway-<node>.events.csv`：快照、反应、恢复、水位和近源 CNP 事件。
- `r3.gateway-<node>.packets.csv`：两端实际发送记录，仅用于离线核验。
- `r3.paths.csv`：流、组和真实数据路径。
- `r3.summary.json`：完成数、全网 admission drops、停止时交换机剩余队列。

`metadata.json` 中 `proposed_parameters.version=3`，记录参数、网关角色及分组。
直接运行 C++ 而未指定 `PROPOSED_OUTPUT` 时，前缀为 `metadata.json.control.csv`。
原有 FCT、goodput、RTT、发送率和 PFC 输出继续保留，CLI 仍只有原六项。

R2 控制器及 `PROPOSED_PREDICTOR`、`PROPOSED_WAN_FRACTION`、`PROPOSED_EPSILON`
已删除，旧配置键会报错。历史研究文档及历史实验结果不改写为 R3 结果。
实现边界、参数单位与验证命令见 [R3 实现说明](PROPOSED_R3_IMPLEMENTATION.md)。
