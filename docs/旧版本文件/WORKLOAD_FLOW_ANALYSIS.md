# Workload 流量架构与算法对比方法分析

> 分析对象：`simulator/ns-3.39/examples/PowerTCP/crossDC-evaluation-workload.cc`
> 配置文件：`examples/PowerTCP/config-workload.txt`
> 拓扑：`examples/PowerTCP/topology.txt`（106 节点 / 42 交换机 / 169 链路，双 DC）
> 查询流：`examples/PowerTCP/query-flow.txt`（12 条固定流）
> CDF：`examples/PowerTCP/Alistorage.txt`

---

## 一、程序流量架构总览

`crossDC-evaluation-workload.cc` 同时生成 **两类流量**，二者角色完全不同：

| 流量类型 | 来源 | 作用 | 是否统计 FCT |
|---------|------|------|-------------|
| **背景流（Background）** | `Alistorage.txt` CDF 随机生成 | 制造可重复的拥塞环境 | 写入 `fct_*.txt`（含 `is_query=0` 标记） |
| **查询流（Query Flow）** | `query-flow.txt` 固定文件 | 被测量的前景流，用于算法对比 | 写入 `query-flow-*.txt`（独立文件） |

程序主流程（`main` 函数尾部）：

```
DiscoverWorkloadTopology()          # 自动发现主机与 leaf
InitializeWorkloadPorts()           # 初始化端口表
InstallBackgroundWorkload(load, cdfTable, ...)   # ① 生成背景流
InstallQueryFlowFile(queryFlowFile, ...)         # ② 生成查询流
Simulator::Run()                    # 运行仿真
```

---

## 二、背景流（Background Flows）深度分析

### 2.1 生成机制

背景流由 `InstallBackgroundWorkload()` 函数生成，核心逻辑（[crossDC-evaluation-workload.cc](simulator/ns-3.39/examples/PowerTCP/crossDC-evaluation-workload.cc#L1011-L1110)）：

1. **按 DC 分离主机**：
   - DC0 主机：`hostId <= 52`（实际主机 ID 为 0–31）
   - DC1 主机：`53 <= hostId <= 105`（实际主机 ID 为 53–84）

2. **DC0 生成流模式**：对每个 DC0 主机，按泊松到达生成流
   ```
   requestRate = load × 1e10 / (8.0 × avgFlowSize × numHostsPerDC)
   ```
   - `load = 0.2`（20% 链路负载）
   - `avgFlowSize` 来自 `Alistorage.txt` CDF 的平均流大小
   - 流大小从 CDF 采样
   - 到达间隔：泊松分布
   - 发射窗口：`START_TIME=0.005s` 到 `FLOW_LAUNCH_END_TIME=0.01s`（仅 5ms 内发射所有背景流）

3. **DC1 镜像**：将 DC0 的流模式 `(src_idx, dst_idx, flowSize, startTime)` **原样复制**到 DC1，保证两个 DC 拥塞场景完全对称。

### 2.2 关键特征

- **纯 DC 内流量**：背景流的 src 和 dst 都在同一个 DC 内（DC0→DC0，DC1→DC1），**不跨 DC**。
- **流大小分布**：来自 `Alistorage.txt`，覆盖 2KB–10MB（见下表）
- **目的随机**：在 DC 内随机选目的主机（排除自身）
- **负载可控**：通过 `LOAD` 参数调节（当前 0.2 = 20%）
- **镜像对称**：两个 DC 的背景流模式完全相同，便于隔离跨 DC 链路的影响

### 2.3 Alistorage CDF 分布

| 流大小 (B) | 累积概率 | 说明 |
|-----------|---------|------|
| 2,000 | 0.0 | 最小流 |
| 10,000 | 0.20 | 20% 的流 ≤ 10KB |
| 20,000 | 0.70 | 70% 的流 ≤ 20KB（短流为主） |
| 50,000 | 0.93 | |
| 100,000 | 0.985 | |
| 1,000,000 | 0.999 | 长尾大流 |
| 10,000,000 | 1.0 | 最大 10MB |

平均流大小约 30–40KB，符合数据中心 short-flow 主导的真实 workload 特征。

### 2.4 背景流的作用

背景流 **不直接用于算法对比**，它的作用是：

1. **制造拥塞**：在 ToR 上行/下行端口、Agg 上行端口产生排队
2. **提供干扰**：让查询流在 realistic 的拥塞环境下完成
3. **可重复性**：固定 `randomSeed=7`，所有算法面对完全相同的背景流模式

---

## 三、Query Flow 深度分析

### 3.1 什么是 Query Flow

Query Flow 是 **被测量的前景流**——它们是性能对比的真正对象。设计意图是模拟"跨数据中心查询/同步"这类对延迟敏感的关键业务流，观察不同拥塞控制算法在背景流干扰下对这些关键流的完成时间影响。

### 3.2 Query Flow 内容（`query-flow.txt`）

共 **12 条流**，每条 **20MB**，格式 `src dst pg dport size start_time`：

| # | src | dst | 类型 | DC | 说明 |
|---|-----|-----|------|-----|------|
| 1 | 0 | 8 | intra-DC | DC0 | 同 ToR34 下 (0-3, 8-11) |
| 2 | 1 | 9 | intra-DC | DC0 | 同 ToR34 下 |
| 3 | 2 | 10 | intra-DC | DC0 | 同 ToR34 下 |
| 4 | 3 | 11 | intra-DC | DC0 | 同 ToR34 下 |
| 5 | 4 | 12 | intra-DC | DC0 | ToR33→ToR35 |
| 6 | 61 | 53 | intra-DC | DC1 | ToR87→ToR85 |
| 7 | 62 | 54 | intra-DC | DC1 | ToR87→ToR85 |
| 8 | 63 | 55 | intra-DC | DC1 | ToR87→ToR85 |
| 9 | 64 | 56 | intra-DC | DC1 | ToR87→ToR85 |
| 10 | 65 | 57 | intra-DC | DC1 | ToR87→ToR86 |
| 11 | 0 | 53 | **cross-DC** | DC0→DC1 | 经 Spine52↔105 跨域链路 |
| 12 | 1 | 53 | **cross-DC** | DC0→DC1 | 经 Spine52↔105 跨域链路 |

**结构**：10 条 intra-DC + 2 条 cross-DC，每个 DC 各 5 条 intra-DC 流。

### 3.3 启动时间调整逻辑

`InstallQueryFlowFile()` 中有一个关键的启动时间调整（[crossDC-evaluation-workload.cc](simulator/ns-3.39/examples/PowerTCP/crossDC-evaluation-workload.cc#L1180-L1190)）：

```cpp
double adjustedStartTime = startTime;  // 原始 0.005s
if (!is_cross_dc && g_longDistanceRtt > 0) {
    // intra-DC 流延迟 1ms 启动（g_longDistanceRtt/2 = 1ms）
    adjustedStartTime += (g_longDistanceRtt / 2.0) / 1e9;
}
```

- **跨 DC 流**：`start_time_ns = 5,000,000`（5ms，原始值）
- **DC 内流**：`start_time_ns = 6,000,000`（6ms，+1ms 调整）

设计意图：让 intra-DC 流的"首包到达"与 cross-DC 流对齐——cross-DC 流首包要经过 1ms 单程传播才能到达目的端，intra-DC 流延后 1ms 启动以同步起跑线。这个调整使两类流的"有效竞争起点"一致。

### 3.4 跨 DC FCT 补偿

`qp_finish()` 中对 cross-DC query flow 的 FCT 做了传播延迟补偿（[crossDC-evaluation-workload.cc](simulator/ns-3.39/examples/PowerTCP/crossDC-evaluation-workload.cc#L420-L440)）：

```cpp
if (cross_dc && g_longDistanceRtt > 0 && fct_ns > g_longDistanceRtt) {
    fct_ns -= g_longDistanceRtt;  // 减去 2ms
}
```

- `g_longDistanceRtt = 2,000,000 ns`（2ms = 跨域链路 1ms × 2，从拓扑 `52↔105 1ms` 检测得到）
- **补偿后**的 FCT 剔除了固定传播延迟，只保留"拥塞 + 序列化"部分
- 这使 cross-DC 与 intra-DC query flow 的 FCT **具有可比性**（都反映算法本身的性能，而非物理距离）

### 3.5 Query Flow 输出格式

独立文件 `query-flow-{algo}_load{load}.txt`：

```
# Format: src_node dst_node pg dport flow_size(B) start_time(ns) fct(ns) is_cross_dc
4 12 3 20035 20000000 6000000 1967205 0       # intra-DC, FCT=1.97ms
0 53 3 20041 20000000 5000000 13111995 1      # cross-DC, FCT=13.11ms (已补偿2ms)
```

字段含义：`src dst pg dport 流大小 起始时间(ns) 完成时间(ns) 是否跨DC`

---

## 四、应该比较哪些流的完成时间

### 4.1 核心结论：比较 Query Flow 的 FCT

**应该比较 query flow 的 FCT，而不是背景流的 FCT。** 原因：

| 维度 | Query Flow | 背景流 |
|------|-----------|--------|
| 角色 | 被测前景流 | 干扰源 |
| 流大小 | 固定 20MB（可控） | 随机 2KB–10MB（不可控） |
| 起点/终点 | 固定（可复现） | 随机（依赖种子） |
| 跨/域 | 明确分类（intra/cross） | 仅 intra-DC |
| FCT 含义 | 算法在拥塞下对关键流的处理能力 | 算法对随机 workload 的整体处理（噪声大） |
| 仿真停止影响 | 有专门的完成计数与提前停止机制 | 可能未完成（截断） |

### 4.2 两类 FCT 输出文件的区别

程序产生两个 FCT 文件，用途不同：

1. **`fct_{algo}_load{load}.txt`**（全量 FCT）
   - 包含 **所有完成的流**（背景流 + query flow）
   - 格式：`sip dip sport dport size start fct standalone_fct is_query`
   - 第 9 字段 `is_query`：1=query flow，0=背景流
   - 用途：辅助分析背景流整体行为，但 **不作为主对比指标**

2. **`query-flow-{algo}_load{load}.txt`**（Query Flow 专用）
   - **只含 query flow**
   - 已做跨 DC 补偿
   - 用途：**算法对比的主要数据源**

### 4.3 为什么不直接比较背景流 FCT

- 背景流大小随机（2KB vs 10MB 的 FCT 差 5000 倍），平均值无意义
- 背景流目的随机，路径长度不同，FCT 方差极大
- 仿真停止时间内部分背景流未完成，样本有截断偏差
- 背景流的作用是"制造拥塞"，本身不是测量对象

---

## 五、当前结果分析与关键问题

> ⚠ 本节完成率与 FCT 数字为 **历史数据**，采集自 `SIMULATOR_STOP_TIME=0.050`（50ms）配置时期。当前 `config-workload.txt` 已将停止时间提升至 `0.080`（80ms），HPCC/TIMELY 的完成率应有改善；如需严谨对比，建议以 80ms（或更高）重跑后更新本节。

### 5.1 Query Flow 完成率（load=0.2，历史数据 stop=50ms）

| 算法 | 完成 / 12 | 完成率 | 说明 |
|------|----------|--------|------|
| FRP (cc13) | 12 | 100% | 全部完成 |
| ROCC (cc14) | 12 | 100% | 全部完成 |
| DCQCN (cc1) | 11 | 91.7% | 缺 `0→8` |
| HPCC (cc3) | 5 | 41.7% | 仅完成 5 条 |
| TIMELY (cc7) | 3 | 25.0% | 仅完成 3 条 |

### 5.2 全量流完成数

| 算法 | 总完成流数 |
|------|-----------|
| FRP | 76 |
| ROCC | 76 |
| DCQCN | 75 |
| HPCC | 69 |
| TIMELY | 67 |

### 5.3 关键问题：仿真停止时间过短

历史上 `SIMULATOR_STOP_TIME = 0.050`（50ms）对 HPCC 和 TIMELY **严重不足**：

- TIMELY 的 query flow FCT 高达 30–36ms，50ms 内只能完成 3 条
- HPCC 的 query flow FCT 高达 13–20ms，50ms 内只能完成 5 条
- 未完成的流无法纳入统计，导致 **样本不完整、对比不公**

**当前状态**：`config-workload.txt` 已将 `SIMULATOR_STOP_TIME` 提升至 `0.080`（80ms），情况有所缓解。若仍需保证所有算法的所有 query flow 都完成，建议进一步提高到至少 `0.2`（200ms）。query flow 有"全部完成后 1ms 提前停止"机制（`g_queryFlowCompleted >= g_totalQueryFlows`），不会因延长停止时间而拖慢快的算法。

### 5.4 Query Flow FCT 示例（FRP, 历史数据 stop=50ms, 已完成全部）

| src→dst | 类型 | FCT (ms) |
|---------|------|----------|
| 4→12 | intra-DC0 | 1.97 |
| 65→57 | intra-DC1 | 2.70 |
| 2→10 | intra-DC0 | 3.31 |
| 3→11 | intra-DC0 | 3.59 |
| 1→9 | intra-DC0 | 6.21 |
| 0→8 | intra-DC0 | 7.57 |
| 61→53 | intra-DC1 | 10.70 |
| 63→55 | intra-DC1 | 10.99 |
| 62→54 | intra-DC1 | 11.20 |
| 64→56 | intra-DC1 | 11.99 |
| 0→53 | cross-DC | 13.11（补偿后） |
| 1→53 | cross-DC | 13.29（补偿后） |

可观察到：
- intra-DC0 流（0→8 等）FCT 1.97–7.57ms，差异来自背景流竞争
- intra-DC1 流（61→53 等）FCT 10.7–12ms，普遍高于 DC0（DC1 可能拥塞更重）
- cross-DC 流（0→53）FCT 13ms，补偿后仍高于 intra-DC，因跨域链路带宽竞争

---

## 六、如何科学比较不同算法的性能

### 6.1 对比维度框架

建议从以下 **五个维度** 对比算法，数据源全部来自 `query-flow-*.txt`：

#### 维度 1：Query Flow 完成率（首要门槛）
```
完成率 = 完成的 query flow 数 / 12
```
- 完成率 < 100% 的算法说明在有限时间内无法处理完关键流，直接判劣
- 这是 **硬性指标**：在真实场景中，query 流超时意味着业务失败

#### 维度 2：Query Flow FCT 统计量（核心指标）
对 **每类** query flow 分别统计：
- **平均 FCT**（mean）
- **中位数 FCT**（P50）
- **P95 / P99 FCT**（尾部延迟）
- **最大 FCT**（worst-case）

分类统计：
- 全部 12 条 query flow
- 10 条 intra-DC query flow
- 2 条 cross-DC query flow

#### 维度 3：FCT 公平性（Fairness）
- 同类 query flow 之间的 FCT 方差/标准差
- 例如 5 条 DC0 intra-DC 流的 FCT 是否接近（公平）还是差异大（饥饿）
- 可用 **Jain's Fairness Index** 量化

#### 维度 4：慢度比（Slowdown）
```
slowdown = actual_fct / standalone_fct
```
- `standalone_fct` = 无拥塞时的理想 FCT（base_rtt + serialization）
- 反映拥塞导致的额外延迟倍数，消除流大小/距离差异
- 注：query-flow 文件未直接输出 standalone_fct，但可从 `fct_*.txt` 中 `is_query=1` 的行获取第 8 字段

#### 维度 5：拥塞指标（辅助）
- 交换机队列长度（`[DCQCN_QLEN]` / `[FRP_DATA_SW]` 日志）
- PFC 触发次数（`pfc_*.txt`）
- 反映算法对拥塞的控制能力，但非 FCT 对比的直接指标

### 6.2 推荐对比流程

```
Step 1: 统一运行条件
  - 所有算法使用相同 load、相同 randomSeed、相同 query-flow.txt
  - 确保 SIMULATOR_STOP_TIME 足够长（建议 ≥200ms）

Step 2: 收集 query-flow-{algo}.txt
  - 每个算法一个文件，12 条流（理想情况）

Step 3: 检查完成率
  - 剔除完成率 < 100% 的算法或标记为"未完成"

Step 4: 计算 FCT 统计量
  - 对每类（全部/intra-DC/cross-DC）算 mean/P50/P95/P99/max

Step 5: 画对比图
  - 柱状图：各算法的 mean/P99 FCT（分组：intra-DC vs cross-DC）
  - 箱线图：各算法 query flow FCT 分布
  - 表格：完整统计量

Step 6: 分析公平性与慢度比（可选深入）
```

### 6.3 具体图表建议

**图 1：Query Flow 平均 FCT 对比（分组柱状图）**
- X 轴：算法（DCQCN / HPCC / TIMELY / FRP / ROCC）
- Y 轴：FCT (ms)
- 分组：intra-DC avg / cross-DC avg / overall avg
- 用途：一眼看出谁快谁慢

**图 2：Query Flow FCT 分布（箱线图）**
- X 轴：算法
- Y 轴：FCT (ms)
- 每个算法一个箱线图，展示 12 条流的分布
- 用途：看公平性和尾部延迟

**图 3：按流 ID 对比的 FCT 热力图/表格**
- 行：12 条 query flow（按 src→dst 标注）
- 列：5 种算法
- 值：FCT (ms)
- 用途：定位具体哪条流在哪个算法下表现差

**图 4：完成率（如有未完成）**
- X 轴：算法
- Y 轴：完成率 %
- 用途：直观展示完成度问题

### 6.4 对比时的注意事项

1. **必须用补偿后的 FCT**：cross-DC query flow 的 FCT 已减去 2ms 传播延迟，直接用 `query-flow-*.txt` 的第 7 字段，不要用 `fct_*.txt` 的原始 FCT

2. **区分 intra-DC 与 cross-DC**：两类流的物理路径差异大，混在一起算平均会被 cross-DC 流拉高，应分开统计

3. **固定 randomSeed**：`--randomSeed=7`，保证背景流模式完全一致，FCT 差异只来自算法

4. **load 作为变量**：可扫 load（0.1/0.2/0.4/0.6/0.8）画 FCT-vs-load 曲线，观察算法在高负载下的退化程度

5. **警惕停止时间截断**：如 5.3 节所述，必须确保所有算法都能完成所有 query flow，否则对比失真

---

## 七、当前实现的问题与改进建议

### 7.1 问题清单

| 问题 | 影响 | 严重度 |
|------|------|--------|
| `SIMULATOR_STOP_TIME=0.05` 过短 | HPCC/TIMELY query flow 未完成，对比失真 | 高 |
| DC0/DC1 背景流完全镜像 | 可能掩盖不对称场景，但利于可重复性 | 低 |
| query flow 仅 12 条 | 样本量小，统计意义弱 | 中 |
| cross-DC query flow 仅 2 条 | 跨 DC 统计样本不足 | 中 |
| intra-DC query flow 启动延迟 +1ms | 设计意图合理但未文档化，易误解 | 低 |
| 背景流 5ms 内全部发射 | 瞬时突发可能不真实 | 低 |

### 7.2 改进建议

1. **延长停止时间**：`SIMULATOR_STOP_TIME` 改为 `0.2` 或更大，配合 query flow 提前停止机制

2. **增加 query flow 数量**：扩展 `query-flow.txt` 到 30–50 条，提高统计显著性，覆盖更多 src/dst 对

3. **增加 cross-DC query flow**：当前仅 2 条（0→53, 1→53），建议增加到 6–10 条，覆盖不同 ToR 对

4. **生成对比脚本**：编写 `plot_workload_fct.py`，自动读取 5 个 `query-flow-*.txt`，输出上述图表

5. **多 load 扫描**：用 `run_all_algos_workload.sh` 配合不同 `LOAD` 值，画 FCT-vs-load 曲线

---

## 八、总结

| 问题 | 答案 |
|------|------|
| 背景流是什么？ | `Alistorage.txt` CDF 生成的 DC 内随机短流，load=0.2，制造拥塞环境，**不跨 DC** |
| Query Flow 是什么？ | `query-flow.txt` 中 12 条固定 20MB 流（10 intra-DC + 2 cross-DC），被测量的前景流 |
| 比较哪些流的 FCT？ | **Query Flow**（用 `query-flow-*.txt`，已做跨 DC 补偿），不比较背景流 |
| 怎么比较算法？ | ①完成率 ②FCT 统计量(mean/P50/P95/P99) ③分 intra/cross-DC ④公平性 ⑤慢度比 |
| 当前最大问题？ | 停止时间 50ms 太短，HPCC/TIMELY 的 query flow 大量未完成 |
