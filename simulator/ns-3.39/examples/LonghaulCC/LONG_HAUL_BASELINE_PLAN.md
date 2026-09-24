# PowerTCP 长距拥塞控制基线实验计划

## 1. 目标与第一阶段范围

在 `examples/LonghaulCC` 中维护一个独立的长距评测程序，参考
`crossDC-evaluation.cc` 的拓扑读取、RDMA 流创建、路由和统计逻辑，对以下三种现有算法建立可重复基线：

| 算法 | `CC` | 第一阶段状态 |
|---|---|---|
| DCQCN | `dcqcn` | 必测 |
| HPCC | `hpcc` | 必测 |
| TIMELY | `timely` | 必测 |

第一阶段只比较算法，不引入 FRP、Bifrost、ATC 等额外机制。所有算法使用相同拓扑、流量、仿真时长、随机种子和统计方法；算法特有参数分别记录，禁止用不同工作负载“调优”结果。

核心问题：

1. 5 ms 单向长距链路下，发送端实际发送速率需要多久稳定？
2. 接收端 goodput 需要多久稳定？发送端与接收端之间有多大时间差？
3. 流加入、退出后能否重新收敛到公平份额？
4. 稳态利用率、公平性、队列、PFC 和完成时间表现如何？

## 2. 已核对的仓库现状

- `crossDC-evaluation.cc` 已支持 DCQCN/HPCC/TIMELY 三种模式，并已有 FCT、PFC、队列以及接收侧 per-flow goodput 的部分统计代码。
- Longhaul 当前在 QP 发包时累计 per-flow payload 与 packet bytes；payload TX 含重传，wire TX 按 Qbb 设备序列化的完整 packet 字节统计。
- 接收 goodput 由接收 QP 接受的按序 payload 计数，独立于发送端 TX；历史 `PrintResultsFlow()` 标准输出不作为 Longhaul 指标输入。
- DCI 输出保留设备 `totalBytesSent` 的区间速率和队列，不输出旧 `rx_bps`；另有逐 flow measured RTT 数据。
- 当前 longhaul 拓扑采用两个互联 DC，每个 DC 有 32 hosts、4 个 leaf、4 个 spine 和 1 个 gateway。
- `topology-256-cross.txt`/`topology.txt` 是每个 DC 256 hosts 的另一套编号（DCI 276/553），不适合作为本阶段目标拓扑。

## 3. 拓扑设计

### 3.1 拓扑文件

当前使用：

```text
topology-longhaul.txt
```

文件名保留原名称以兼容已有配置和 runner，但文件内容已经改为较小的
两层 Leaf-Spine 拓扑：

- DC0 hosts：`0..31`，leaf：`32..35`，spine：`36..39`，gateway：`40`；
- DC1 hosts：`41..72`，leaf：`73..76`，spine：`77..80`，gateway：`81`；
- 两侧各 4 个 leaf、4 个 spine、1 个 gateway；每个 leaf 连接 8 个 host；
- 总节点数 82，交换机数 18，host 数 64，链路数 105；
- 每个 DC 内 leaf-spine 全连接，spine-gateway 全连接；
- 内部链路保持 100 Gbps、1.5 us；
- DCI 链路唯一且为：

```text
40 81 200000000000.0 5ms 0
```

在当前 Qbb 点到点信道模型中，一条链路的 `Delay=5ms` 对两个传输方向分别生效，因此跨 DC RTT 至少约为 10 ms；这符合“单向 5 ms”的含义。

节点 ID 采用连续分段，便于人工检查；程序仍然以拓扑文件中的交换机列表和链路为准，
不应在分析脚本中根据节点 ID 范围推断路径属性。

## 4. 程序与文件布局

建议新增而不是继续堆叠修改 `crossDC-evaluation.cc`：

```text
longhaul-convergence.cc              # 仿真入口和公共默认值
config-longhaul-common.txt           # 完整、可审阅的程序配置
flow-longhaul-*.txt                  # 固定场景流文件
topology-longhaul.txt                # 新拓扑
run-longhaul.py                      # 指定算法、seed/run、目录管理
run-longhaul-all.py                  # 默认运行全部算法
analyze-longhaul.py                  # 收敛判定、汇总和绘图
```

同时在 `CMakeLists.txt` 中注册 `longhaul-convergence`。公共参数由 C++ 默认值提供，完整配置文件覆盖默认值，显式命令行参数再覆盖前两者。切换场景时由命令行传入 `FLOW_FILE` 和 `SIMULATOR_STOP_TIME` 对应的参数；场景名从 flow 文件名生成。输出路径由 runner 写入每次运行的 `config.txt`，算法、seed/run 等批量运行时参数由 runner 通过六个命令行入口传入；原始配置文本保持不变。

所有输出目录由 runner 预先创建；C++ 程序遇到文件无法打开时应立即报错退出，不能像现有 FCT 路径那样静默跳过。

## 5. 测量设计

config-longhaul.txt

流级 CSV 用 `interval_start_ns,time_ns` 表示采样区间，并用五元组字段区分 flow；DCI
链路 CSV 用方向和端点标识链路：

```text
sender-rate.csv: algorithm,seed,run,interval_start_ns,time_ns,src,dst,sport,dport,pg,tx_payload_bytes,tx_wire_bytes,tx_payload_bps,tx_wire_bps
receiver-goodput.csv: algorithm,seed,run,interval_start_ns,time_ns,src,dst,sport,dport,pg,goodput_bytes,goodput_bps
dci-link.csv: algorithm,seed,run,time_ns,src,dst,direction,tx_bps,queue_bytes,ecn_events,pfc_pause_events,pfc_resume_events
measured-rtt.csv: algorithm,seed,run,interval_start_ns,time_ns,src,dst,sport,dport,pg,sample_count,rtt_min_ns,rtt_mean_ns,rtt_p50_ns,rtt_p95_ns,rtt_max_ns
```

- **发送端实际速率**：在 QP packet 调用 `QbbNetDevice::TransmitStart()` 时累计实际 payload 和完整 packet bytes；payload rate 包含重传，wire rate 包含该算法实际使用的协议和 INT 头。
- **接收端 goodput**：使用 `RdmaRxQueuePair::m_recv_bytes` 的按序有效 payload 增量；不含重复包、重传重复字节或控制包，并在完成时补记不足一个采样周期的尾窗。
- **算法内部速率（辅助）**：可另外记录统一的 `qp->m_rate`。DCQCN 的 `mlx.m_targetRate`、HPCC 的 `hp.m_curRate`、TIMELY 的 `tmly.m_curRate` 仅用于诊断，不能替代实际发送速率。
- **DCI 链路**：`tx_bps` 记录两个方向 DCI 设备 `totalBytesSent` 的区间增量，按 ns-3 packet bytes 计算，保留队列字节及 ECN/PFC 事件；不输出语义不成立的 `rx_bps`。
- **RTT**：从发送端 NIC 开始发送到发送端收到推进累计 ACK 计时；排除无法区分重传发送时刻的样本，并按配置周期汇总分位数。
- **完成事件**：保留 FCT 文件，并加上 node ID、场景名和算法名。

对每条 flow 单独计算 base RTT、跳数、路径瓶颈和 BDP。base RTT 为正反向链路传播时延与
每跳 NIC 接收处理延迟之和，不含排队和序列化；BDP/window 为该 flow 路径瓶颈带宽乘以
其 base RTT 再除以 8。`standalone_fct` 使用同一条路径：base RTT、首个数据包逐跳序列化、
剩余 packet wire bytes 在瓶颈链路上的序列化，以及最终 ACK 的反向逐跳序列化；头部大小与
`QbbNetDevice` 实际序列化的 PPP 点到点 packet 一致，不含排队、丢包和竞争。

建议先以 100 us 采样。它相对约 10 ms RTT 足够细，同时比当前 `1.5 * minRtt` 更可控；最终绘图可用 0.5 或 1 ms 滑动窗降噪，但收敛计算必须注明使用的是原始序列还是平滑序列。

### 5.2 收敛时间的统一定义

对一个稳定活跃流数为 `N` 的阶段，理论公平份额先定义为：

```text
R_target_payload = min(path_bottleneck, DCI_rate) / N
                   × payload_bytes / (payload_bytes + data_header_bytes)
```

本地 flow 将 DCI_rate 替换为自身路径瓶颈。packet 头部使用 Qbb 模型大小，含 14 字节
`PppHeader`、IPv4、UDP、序号和活动 INT 头。

若 host NIC 或路径上的其他链路更窄，则使用路径瓶颈容量除以共享该瓶颈的活跃流数。分析脚本应从场景清单获得活跃区间，不能从结果反推实验阶段。

默认判据：从阶段变化时刻 `t0` 起，速率首次进入目标的 ±10%，并连续保持至少 `max(3 * base_RTT, 20 ms)`，该窗口起点减 `t0` 即 settling time。

分别计算：

- `sender_settling_ms`：发送端实际速率的收敛时间；
- `receiver_settling_ms`：接收端 goodput 的收敛时间；
- `observation_lag_ms = receiver_settling_ms - sender_settling_ms`；
- 未在阶段结束前满足判据时标记 `not_converged`，禁止用阶段结束时间代替。

同时报告 ±5% 和 ±20% 灵敏度，避免结论依赖单一阈值。

### 5.3 其他指标

- 稳态平均速率、P5/P50/P95 速率；
- DCI 利用率和稳态有效吞吐；
- Jain fairness index（瞬时曲线和稳态值）；
- overshoot、最大 undershoot、振荡幅度、变异系数；
- 队列 P50/P95/P99/max，ECN/CNP 和 PFC pause 次数/时长；
- FCT 与 normalized FCT（仅适用于会结束的探测流）；
- 丢包/重传或 NACK 数量（若模型可直接观测）；
- 每次运行的 wall-clock 时间和输出规模。

## 6. 第一阶段场景矩阵

先用少量、解释性强的确定性场景建立基线：

| 场景 | 流量安排 | 目的 |
|---|---|---|
| S0 单流 | 1 条 DC0→DC1 长流，持续至少 30 RTT | 无竞争时爬升、链路利用率、基础 RTT |
| S1 同步竞争 | 8 条同向长流在同一时刻启动，源/宿分散到 4 个 leaf | 初始收敛、公平性、incast 偏差控制 |
| S2 流加入 | 1 条先运行 20 RTT，再同时加入 7 条，之后运行至少 30 RTT | 降速响应和新公平点收敛 |
| S3 流退出 | 8 条先稳定，其中 7 条以有限字节数近同时结束，余下 1 条继续至少 30 RTT | 带宽再获取速度 |
| S4 双向 | 两方向各 8 条长流 | 对称性及 ACK/反馈交互 |
| S5 短流叠加 | 稳定长流上周期性加入有限大小探测流 | FCT 与长流稳定性的折中 |

第一轮 smoke test 只跑 S0、S2；通过后再跑完整矩阵。每条长流的字节数必须保证覆盖整个测量区间，仿真停止时间至少包含 warm-up、阶段变化和最后一个保持窗口。

每种算法每个场景建议 5 个 `RngRun`。即使链路误码为 0，ECMP/哈希及后续随机工作负载也需要固定并记录 seed/run。第一阶段先固定 `RngSeed=1`，使用 `RngRun=1..5`。

## 7. 公平比较约束

- 三种算法只改变 `CC` 和明确列出的算法专属参数。
- 拓扑、流文件、包大小、PFC/ECN、buffer、ACK 优先级、采样周期、seed/run 必须一致。
- 配置中补齐 100 Gbps 和 200 Gbps 的 `KMIN_MAP/KMAX_MAP/PMAX_MAP`；当前模板不应在开启 multi-rate/ECN 时缺少 200 Gbps 项。
- HPCC 需要 INT，TIMELY 依赖 RTT，DCQCN 依赖 ECN/CNP；报告中应明确这是算法机制差异，而不是把头部开销或反馈流量删掉。
- 长距 BDP 很大。`HAS_WIN`、`VAR_WIN` 会直接限制窗口；每条 flow 使用自己的 base RTT 和路径瓶颈计算 BDP，并在 S0 验证窗口不会把吞吐错误地卡在拥塞控制速率以下。
- `SIMULATOR_STOP_TIME`、流大小、采样窗均以 RTT 倍数检查；不能沿用 0.15 s 就默认足够。
- 禁止从标准输出正则解析最终数据；stdout 只保留运行日志，指标写结构化文件。

## 8. 实施步骤

### Phase A：拓扑与最小可运行程序

1. 使用 82-node 拓扑，确认 DCI delay 为 5 ms。
2. 从 `crossDC-evaluation.cc` 复制出独立入口，删除与本实验无关的 Bifrost/特殊硬编码路径。
3. 注册 CMake target，使用 S0 + DCQCN 完成编译和最短运行。
4. 在 metadata 中按 flow 检查跨 DC base RTT：包含正反向 5 ms DCI 传播及内部传播/NIC 接收延迟，而不是把单向 5 ms 当作 RTT。

### Phase B：可靠观测

1. 把 sender payload/wire rate、receiver goodput、DCI TX throughput/queue 和 measured RTT 写成独立 CSV。
2. 处理 QP 创建前、首次采样、QP 完成删除和尾窗统计；验证 DCI 首个采样窗从仿真起点计时。
3. 用单流无拥塞案例做守恒检查：发送累计 payload、接收累计 payload、FCT 文件三者一致。
4. 检查 100 us 采样对仿真运行时间和文件大小的影响。

### Phase C：三算法基线

1. 固化三份算法参数清单和公共配置哈希。
2. 跑 S0、S2 smoke matrix，自动检查 crash、空文件、NaN、吞吐超过物理链路等问题。
3. 跑 S0–S5、3 algorithms、5 runs 的完整矩阵。
4. 分析脚本输出 per-run summary 和跨 run 的 mean/median/95% CI。

### Phase D：图表与验收

至少生成：

1. sender rate vs time（标出阶段变化和公平目标线）；
2. receiver goodput vs time；
3. sender/receiver settling time 对比；
4. DCI aggregate utilization、queue vs time；
5. Jain fairness vs time；
6. 三算法稳态吞吐、振荡、PFC、FCT 汇总图。

每张图旁边保留对应 CSV、配置快照、git commit、命令行、seed/run 和分析参数。

## 9. 验收标准

- 新拓扑自动校验全部通过，确认为每 DC 32 hosts、DCI 40↔81、200 Gbps、单向 5 ms。
- 三种 `CC` 在 S0 和 S2 均能完成运行，不出现 assertion、空结果或负吞吐。
- 单流累计发送/接收字节误差可解释，稳态吞吐不超过路径瓶颈。
- 同一配置和 seed/run 重跑得到相同结构与相同数值（允许格式化舍入误差）。
- sender 和 receiver 收敛时间由脚本按同一规则计算，未收敛明确标注。
- 结果目录可由一条 runner 命令重建，分析和绘图不依赖人工复制日志。

## 10. TODO Checklist

### 拓扑

- [x] 使用 82 个节点、18 个交换机和 105 条链路。
- [x] 每个 DC 配置 32 个 host、4 个 leaf、4 个 spine 和 1 个 gateway。
- [x] 将 header 设置为 82 nodes、18 switches、105 links。
- [x] 将 40↔81 设置为 200 Gbps、5 ms、error rate 0。

### C++ 程序

- [ ] 新建 `longhaul-convergence.cc`。
- [ ] 删除端口号、ToR 号、receiver 号硬编码。
- [ ] 增加 seed/run 和采样/输出配置项。
- [ ] 输出 sender actual rate CSV。
- [ ] 输出 receiver goodput CSV。
- [ ] 输出双向 DCI link/queue CSV。
- [ ] 保留并扩展 FCT、PFC 输出。
- [ ] 文件打开失败时 fail-fast。
- [ ] 在 `CMakeLists.txt` 注册 target 并编译。

### 场景与配置

- [ ] 创建 S0–S5 固定流文件。
- [ ] 通过 `--flow-file` 和 `--stop-time` 选择场景，不为场景复制配置文件。
- [ ] 通过 `--cc=dcqcn|hpcc|timely` 选择算法，不为算法复制配置文件。
- [ ] 补齐 100/200 Gbps ECN map。
- [ ] 验证长距 BDP/window 配置不会人为限速。
- [ ] 固定 packet size、buffer、PFC、ACK priority 和 stop time。

### 自动化与分析

- [ ] runner 创建结果目录并记录 metadata/config hash/git commit。
- [ ] 实现 ±10% + 保持窗口收敛判定。
- [ ] 增加 ±5%/±20% 灵敏度分析。
- [ ] 计算 utilization、Jain fairness、overshoot、振荡、queue、PFC、FCT。
- [ ] 自动生成 6 类基线图。
- [ ] 先完成 S0/S2 smoke test，再启动完整 90 次运行（6 场景 × 3 算法 × 5 runs）。

## 11. 开始编码前需锁定的口径

本计划默认采用以下解释，若需求不同应在实现前修改计划：

1. “单向 5 ms”指 Qbb channel 的 `Delay=5ms`，所以往返传播时延约 10 ms。
2. “发送端速率”分为实际发出的 payload bit rate（含重传）和 Qbb packet bytes bit rate；算法内部 `qp->m_rate` 只作辅助诊断。
3. 当前拓扑采用每个 DC 4 leaf/4 spine/1 gateway 的两层 Leaf-Spine 结构，每个 leaf 连接 8 个 host。
4. 第一阶段主瓶颈是唯一的 200 Gbps DCI；内部链路保持 100 Gbps。
