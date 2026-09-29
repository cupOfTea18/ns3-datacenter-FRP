# 拓扑结构与拥塞点说明

> 数据来源：`simulator/ns-3.39/examples/PowerTCP/topology.txt`、`config-workload.txt`、`query-flow.txt`、`Alistorage.txt`
> 主程序：`simulator/ns-3.39/examples/PowerTCP/crossDC-evaluation-workload.cc`
> 拓扑首行：`106 42 16 169`（106 节点 / 42 交换机 / 16 ToR / 169 链路）

## 一、拓扑结构说明

本拓扑是一个 **双数据中心（DC1 / DC2）** 的胖树（leaf-DCI）变体，每个 DC 内部为三层（ToR → Agg → Core）+ 一个DCI交换机，两个 DC 的 DCI交换机 之间通过一条高延迟链路互联。所有 DC 内链路均为 **100Gbps / 1.5us**，跨 DC 链路为 **200Gbps / 1ms**（`topology.txt` 第 171 行 `52 105 200000000000.0 1ms 0`）。

### 1.1 层次划分

| 层级 | DC1 节点 ID | DC2 节点 ID | 说明 |
|------|------------|------------|------|
| 主机 | 0–31 | 53–84 | 每个主机 100Gbps 上行到 ToR |
| ToR | 32–39 | 85–92 | 每 DC 8 个 ToR，每个 ToR 接 4 台主机 |
| Agg | 40–47 | 93–100 | 每 DC 8 个 Agg |
| Core | 48–51 | 101–104 | 每 DC 4 个 Core |
| DCIe | 52 | 105 | 每 DC 1 个 DCI，连接所有 Core |

### 1.2 DC 内部连接关系（以 DC1 为例，DC2 完全对称）

- **主机 → ToR**：每 4 台主机连 1 个 ToR
  - 0–3 → ToR32，4–7 → ToR33，8–11 → ToR34，12–15 → ToR35
  - 16–19 → ToR36，20–23 → ToR37，24–27 → ToR38，28–31 → ToR39
- **ToR ↔ Agg**（1.5us，2:1 全连接，每个 ToR 连 2 个 Agg，每个 Agg 连 2 个 ToR）：
  - 32↔40,47；33↔40,41；34↔41,42；35↔42,43；36↔43,44；37↔44,45；38↔45,46；39↔46,47
- **Agg ↔ Core**（1.5us，全连接，每个 Agg 连 4 个 Core，每个 Core 连 8 个 Agg）：
  - 40–47 各自连接 48,49,50,51
- **Core ↔ DCI**（1.5us）：48,49,50,51 各自连接 52

DC2 对称：ToR 85–92，Agg 93–100，Core 101–104，DCI 105。

### 1.3 跨 DC 互联

```
DCI52 ──── 200Gbps / 1ms ──── DCI105
```

这是唯一的跨 DC 链路，延迟比 DC 内链路高约 **667 倍**（1ms vs 1.5us），是跨 DC 流的带宽瓶颈与 RTT 主导因素。程序在启动时扫描拓扑检测到该链路（`delay > 100000ns`），设置 `g_longDistanceRtt = 2,000,000 ns`（2ms = 单程 1ms × 2），用于 cross-DC query flow 的 FCT 补偿。

### 1.4 IP 分配（由 `AutoAssignTopologyIps` 自动生成）

- 掩码 `255.255.0.0`，IP 规则 `11.{dcId}.{localId/256}.{localId%256}`
- BFS 以 1ms 为阈值划分 DC：DC 内链路（1.5us）归同一 DC，跨 DC 链路（1.5ms）切断
- DC1 → `dcId=0`，DC2 → `dcId=1`
- 每个 DC 内交换机/主机按发现顺序从 `localId=1` 递增分配 IP

### 1.5 拓扑示意

```
        ┌──────────────── 跨DC链路 (200G/1.5ms) ────────────────┐
      DCI52                                              DCI105
      /  |  |  \                                          /  |  |  \
   48  49  50  51                                     101 102 103 104
    |\ | /| /| /|                                       |\ | /| /| /|
    Agg 40-47 (×8)                                     Agg 93-100 (×8)
    |  / |  / |  /                                      |  / |  / |  /
   ToR 32-39 (×8)                                     ToR 85-92 (×8)
    |    |    |                                         |    |    |
  主机0-31 (×32)                                     主机53-84 (×32)
```

## 二、流量设置

> 当前主程序为 `crossDC-evaluation-workload.cc`，配置文件为 `config-workload.txt`。
> 流量分为 **背景流**（CDF 随机生成，制造拥塞）与 **查询流**（`query-flow.txt` 固定文件，被测量的前景流）两类。

### 2.1 背景流（Background，`Alistorage.txt` CDF）

背景流由 `InstallBackgroundWorkload()` 生成，**纯 DC 内流量**，不跨 DC：

- **CDF 来源**：`examples/PowerTCP/Alistorage.txt`，流大小分布 2KB–10MB，平均约 30–40KB，短流为主。
- **负载**：`LOAD=0.2`（20%），到达率 `requestRate = load × 1e10 / (8.0 × avgFlowSize × numHostsPerDC)`，泊松到达。
- **发射窗口**：`START_TIME=0.005s` 到 `FLOW_LAUNCH_END_TIME=0.01s`（5ms 内全部发射）。
- **DC 镜像**：DC0（主机 0–31）先生成流模式 `(src_idx, dst_idx, flowSize, startTime)`，DC1（主机 53–84）**原样镜像**，保证两个 DC 拥塞场景完全对称。
- **目的随机**：在 DC 内随机选目的主机（排除自身），路径为 DC 内 ToR→Agg→ToR。
- **作用**：制造可重复的拥塞环境，干扰查询流，**不作为算法对比指标**。

### 2.2 查询流（Query Flow，`query-flow.txt`）

查询流是被测量的前景流，共 **12 条**，每条 **20MB**，格式 `src dst pg dport size start_time`：

| # | src→dst | 源 ToR | 目的 ToR | 类型 | DC | start(ns) |
|---|---------|--------|---------|------|-----|-----------|
| 1 | 0→8 | ToR32 | ToR34 | intra-DC | DC0 | 6,000,000 |
| 2 | 1→9 | ToR32 | ToR34 | intra-DC | DC0 | 6,000,000 |
| 3 | 2→10 | ToR32 | ToR34 | intra-DC | DC0 | 6,000,000 |
| 4 | 3→11 | ToR32 | ToR34 | intra-DC | DC0 | 6,000,000 |
| 5 | 4→12 | ToR33 | ToR35 | intra-DC | DC0 | 6,000,000 |
| 6 | 61→53 | ToR87 | ToR85 | intra-DC | DC1 | 6,000,000 |
| 7 | 62→54 | ToR87 | ToR85 | intra-DC | DC1 | 6,000,000 |
| 8 | 63→55 | ToR87 | ToR85 | intra-DC | DC1 | 6,000,000 |
| 9 | 64→56 | ToR87 | ToR85 | intra-DC | DC1 | 6,000,000 |
| 10 | 65→57 | ToR88 | ToR86 | intra-DC | DC1 | 6,000,000 |
| 11 | 0→53 | ToR32 | ToR85 | **cross-DC** | DC0→DC1 | 5,000,000 |
| 12 | 1→53 | ToR32 | ToR85 | **cross-DC** | DC0→DC1 | 5,000,000 |

**结构**：10 条 intra-DC（DC0 五条、DC1 五条）+ 2 条 cross-DC。

**启动时间调整**：cross-DC 流用原始 `0.005s` 启动；intra-DC 流延迟 1ms 启动（`+g_longDistanceRtt/2`），使两类流"首包到达目的端"的时间对齐——cross-DC 流首包需 1ms 单程传播，intra-DC 流延后 1ms 起跑，有效竞争起点一致。

**FCT 补偿**：cross-DC query flow 的 FCT 在输出时减去 `g_longDistanceRtt=2ms`，剔除固定传播延迟，只保留"拥塞 + 序列化"部分，使 cross-DC 与 intra-DC 的 FCT 可比。

### 2.3 主机到 ToR 映射（路径分析依据）

| ToR | DC0 主机 | ToR | DC1 主机 |
|-----|---------|-----|---------|
| 32 | 0–3 | 85 | 53–56 |
| 33 | 4–7 | 86 | 57–60 |
| 34 | 8–11 | 87 | 61–64 |
| 35 | 12–15 | 88 | 65–68 |
| 36 | 16–19 | 89 | 69–72 |
| 37 | 20–23 | 90 | 73–76 |
| 38 | 24–27 | 91 | 77–80 |
| 39 | 28–31 | 92 | 81–84 |

ToR↔Agg 连接（每个 ToR 连 2 个 Agg）：32↔40,47；33↔40,41；34↔41,42；35↔42,43；36↔43,44；37↔44,45；38↔45,46；39↔46,47（DC2 对称：85↔93,100；86↔93,94；87↔94,95；88↔95,96 …）。Agg↔Core 全连接（每个 Agg 连 4 个 Core），Core↔DCI 全连接。

## 三、拥塞点分析

### 3.1 拥塞点 1（最严重）：ToR85 → 主机53 下行链路（incast）

```
query flow 汇聚到主机 53:
  0→53  (cross-DC, ToR32→...→ToR85→53)
  1→53  (cross-DC, ToR32→...→ToR85→53)
  61→53 (intra-DC1, ToR87→ToR85→53)
            ↓ 3 条 20MB 流 + 背景流
         ToR 85
            ↓ 仅 100Gbps 下行
         主机 53
```

- **3:1 的 query flow 汇聚**：3 条 20MB 流（60MB 总量）争抢 100Gbps 下行链路，理论最快约 4.8ms，实际因拥塞显著更长。
- 这是当前场景**最严重的 incast 热点**，也是 cross-DC 与 intra-DC 流交汇的唯一节点。
- **代码已针对性处理**：`crossDC-evaluation-workload.cc` 对 **SW93（Agg，ToR85 的上行 Agg 之一）port 1** 做了特殊 PFC 配置（threshold=2MB、headroom=100KB、xon=256KB、PauseTime=100ms），保护 ToR85→host53 路径，防止队列堆到 7MB+ 才丢包。

### 3.2 拥塞点 2：ToR32 上行（6 条 query flow 同源）

```
主机 0,1,2,3 (同在 ToR32 下) 同时发出:
  0→8, 1→9, 2→10, 3→11  (4 条 intra-DC0 → ToR34)
  0→53, 1→53             (2 条 cross-DC → ToR85)
```

- 主机 0、1 各发 2 条 20MB 流，主机 2、3 各发 1 条，ToR32 上行瞬时承受 6 条 query flow + 背景流。
- ToR32 上行 100Gbps，6 条流理论过载 6:1，但分散到不同目的（ToR34、ToR85），实际由 ECMP 分散到 Agg40/47。

### 3.3 拥塞点 3：ToR34 下行（4 条并行 query flow）

```
0→8, 1→9, 2→10, 3→11  全部 ToR32 → ToR34
            ↓ 4 条 20MB 流
         ToR 34
            ↓ 分别到主机 8,9,10,11 (每台 1 条, 非 incast)
```

- 4 条流目的主机不同（8/9/10/11），不是 incast，但 4 条流同时到达 ToR34 并竞争 ToR34 的入向端口（来自 Agg41/42）。
- ToR34 下行 4 条 100Gbps 链路（到 8/9/10/11）各自只收 1 条流，下行无拥塞；拥塞出现在 ToR34 的 Agg 入向端口。

### 3.4 拥塞点 4：ToR87 上行（4 条 query flow 同源）

```
主机 61,62,63,64 (同在 ToR87 下) 同时发出:
  61→53, 62→54, 63→55, 64→56  (4 条 intra-DC1 → ToR85)
```

- 与 ToR32 对称，4 台主机同时发 20MB 流，ToR87 上行 100Gbps 承受 4:1 过载 + 背景流。
- 这 4 条流中 61→53 会汇入拥塞点 1（host53 incast），其余 3 条到 54/55/56 无 incast。

### 3.5 拥塞点 5：跨 DC 链路 DCI52↔105（2 条 cross-DC query flow）

```
0→53, 1→53  (cross-DC) 必经 DCI52 ↔ DCI105
            ↓ 2 条 20MB 流 + 无背景流(背景流纯 DC 内)
   200Gbps / 1ms 跨 DC 链路
```

- 跨 DC 链路 200Gbps，仅 2 条 cross-DC query flow（无背景流跨 DC），带宽充足（2×100Gbps 源 < 200Gbps）。
- **瓶颈不在带宽，而在 RTT**：1ms 单程传播使 cross-DC 流的 BDP 高达 25MB（200Gbps × 1ms / 8），拥塞控制收敛慢。
- cross-DC 流还与 intra-DC1 流（61→53）在 ToR85→53 下行争抢（见拥塞点 1）。

### 3.6 不产生显著拥塞的位置

- **ToR85 下行到 54/55/56/57**：各只收 1 条 query flow，无 incast。
- **DC 内 Agg/Core/DCI 上行**：背景流 load=0.2 较轻，且 ECMP 分散，不形成持久瓶颈。
- **跨 DC 链路带宽**：200Gbps 远超 2 条流的需求。

## 四、监控建议

按 `copilot-instructions.md` 的约定，队列/速率分析依赖硬编码的日志标签和交换机端口：

- **`[DCQCN_QLEN]`**（ccMode 1/3/7）和 **`[FRP_DATA_SW]`**（ccMode 13/14）输出队列长度，由 `switch-node.cc` 产生。
- **`[FLOW TP]`** 和 **`[FRP RATE]`** 由 `rdma-hw.cc` 产生，反映 per-flow 接收吞吐。
- **DCQCN 路径的硬编码限制**：`switch-node.cc` 的 `SwitchNotifyDequeue()` 中 `if (m_id == 32 || m_id == 85 || m_id == 93)` 限定 `[DCQCN_QLEN]` 只输出 ToR32、ToR85、SW93 三个交换机；要监控其他交换机必须改此处并重新构建。FRP/ROCC 路径的 `[FRP_DATA_SW]` 对所有交换机输出，无此限制。

### 4.1 应监控的交换机与端口

| 交换机 | 端口 | 连接 | 拥塞类型 | 优先级 |
|--------|------|------|---------|--------|
| **ToR 85** | 下行到 host53 | ToR85→53 | incast（3 条 query flow） | 最高 |
| **ToR 85** | 上行到 Agg93/100 | Agg→ToR85 | cross-DC + intra-DC 汇聚 | 高 |
| **ToR 32** | 上行到 Agg40/47 | ToR32→Agg | 6 条 query flow 同源 | 高 |
| **ToR 34** | 上行到 Agg41/42 | Agg→ToR34 | 4 条 query flow 入向 | 中 |
| **ToR 87** | 上行到 Agg94/95 | ToR87→Agg | 4 条 query flow 同源 | 中 |
| **SW 93** (Agg) | port 1 | SW93→ToR85 | 已特殊配置 PFC，观察配置效果 | 中 |
| **DCI 52/105** | 跨 DC 端口 | 52↔105 | cross-DC 流 RTT 瓶颈 | 低（带宽不缺） |

### 4.2 当前监控脚本的不足

- `run_single_workload_algo.py` 自动检测拥塞点（按平均队列长度排序取 top 5），无需硬编码端口，适应性较好。
- 但若需精确定位上述 incast 热点，建议在日志中过滤 `Switch 85` 的端口队列数据，确认 host53 下行是否为最深队列。

## 五、各算法在此场景下的预期行为

| 算法 | ccMode | 在本 workload 场景的预期表现 |
|------|--------|--------------------------|
| DCQCN | 1 | 依赖 ECN 标记降速，incast 下反应较慢，host53 汇聚易触发 PFC，query flow 尾延迟较高 |
| HPCC | 3 | 通过 INT 精确感知队列，收敛较快，但 ECMP 下多路径使 INT 反馈可能抖动 |
| TIMELY | 7 | 基于 RTT 的速率调整，cross-DC 流 RTT 大（1ms+），速率调整周期长，收敛慢 |
| FRP | 13 | 周期性反馈（40us）维护公平速率公式，在瓶颈链路实现公平共享，cross-DC 下需跨域反馈 |
| ROCC | 14 | 类似 FRP 的反馈机制，交换机端主动调度 |

**实测完成率参考**（load=0.2，历史数据，stop=50ms 配置时期）：FRP/ROCC 完成 12/12，DCQCN 11/12，HPCC 5/12，TIMELY 3/12。HPCC/TIMELY 未完成主因是停止时间过短，而非算法本身不可用。当前 `config-workload.txt` 的 `SIMULATOR_STOP_TIME` 已提升至 `0.080`（80ms），并配合查询流"全部完成后 1ms 提前停止"机制；如仍需更完整的 HPCC/TIMELY 样本，建议进一步提高到 0.2s——详见 [WORKLOAD_FLOW_ANALYSIS.md](WORKLOAD_FLOW_ANALYSIS.md)。