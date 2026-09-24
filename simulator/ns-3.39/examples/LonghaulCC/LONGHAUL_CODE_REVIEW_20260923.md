# LonghaulCC 程序审查报告

审查日期：2026-09-23  
审查对象：`longhaul-convergence.cc` 当前工作区版本，以及它调用的 RDMA、交换机、INT、配置和默认流文件。  
范围：DCQCN、HPCC、TIMELY、FRP 基准及代码简洁性/效率；按要求不审 Proposed。  
方式：静态代码审查；没有修改程序、配置或实验结果，也没有运行构建或仿真。

## 结论

**当前不能把四个算法都作为正确可信的基准使用。** 有两类问题：算法在当前拓扑/参数下未正确适配；默认流量超过底层序号表示能力。此外，部分观测量的名称和计算口径会误导结果分析。

| 基准 | 当前代码状态 | 审查结论 |
|---|---|---|
| DCQCN | ECN 标记能通过带 CNP 标志的 ACK 到达发送端，Mellanox 风格的 alpha、降速及恢复代码存在 | 能运行的是简化模型；当前反馈路径不等同完整 DCQCN CNP 行为。若按论文基准报告，需标明适配并验证 CNP/降速/恢复闭环 |
| HPCC | mode=3 启用 INT，逐交换机追加遥测 | 当前跨域路由经过 6 台交换机，INT 与发送端遥测数组最多保存 5 跳；首次反馈可能越界，结果无效 |
| TIMELY | mode=7 使用发送端时间戳并由 ACK 回显计算 RTT | 当前 `THigh=10 ms`，而跨域仅传播 RTT 已为 10.018 ms；无拥塞时也会进入乘性降速分支，结果无效 |
| FRP | 算法实现分散在 RDMA/交换机代码中 | 当前入口拒绝 `frp`，且没有启动交换机周期反馈；数据中心分类地址也不匹配本程序的地址规划，尚不是可运行基准 |

另外，默认 S0 流量为 1 TB；传输序号和接收端期望序号为 32 位，不能正确完成超过 4 GiB 的流。S0、S1、S2、S4、S5 都使用 1 TB，不能作为当前四算法的正式完成时间基准。

## 会使算法结果失效的问题

### P1：默认 1 TB 流超过传输序号范围

位置：`flow-longhaul-s0.txt:1-2`；`flow-longhaul-s1.txt`、`s2`、`s4`、`s5`；`rdma-hw.cc:646-690`；`rdma-queue-pair.h:22-23,160`；`qbb-header`/`SeqTsHeader` 的 32 位序号字段。

QP 的 `snd_nxt` 是 64 位，但每个数据包的序号经 `SeqTsHeader::SetSeq(uint32_t)` 截为 32 位；接收端 `ReceiverNextExpectedSeq` 和 ACK 序号也只有 32 位。发送端 ACK 处理再把该 32 位 ACK 与 64 位 `snd_una` 作普通大小比较，没有序号回绕处理。超过 `2^32` 字节后，ACK 回绕为小数值，不再推进发送端 `snd_una`。此外，`GetNxtPacket()` 将 64 位剩余字节先转为 `uint32_t`；剩余字节落在 32 位取值为零的位置时，数据包载荷变成零，发送进度无法前进。

**影响：** 1 TB 流不能完成；只看仿真正常退出或停止前的吞吐曲线，不能证明流和控制器有效。`flow-longhaul-s3.txt` 的 625 MB 低于 32 位序号上限；`flow-longhaul-small.txt` 的 100 MB 也低于上限，但需按实际任务设定停止时间。

**最小处理方向：** 当前使用小于 4 GiB 的有限流；更稳妥的是给 32 位序号留出余量。若实验必须传超过 4 GiB，再统一扩展数据序号、ACK、接收端期望序号和重传比较，不能只改单处类型。

### P1：HPCC 的 6 跳遥测写入 5 跳数组

位置：`topology-longhaul.txt`；`int-header.h:82-99`、`int-header.cc:28-34`；`switch-node.cc:355-365`；`rdma-hw.cc:905-930`。

默认跨域路径包含源 leaf、源 spine、源 gateway、目的 gateway、目的 spine、目的 leaf，共 6 台交换机。`IntHeader::maxHop` 和 `qp->hp.hop` 的容量都是 5；`PushHop()` 在第六跳用模运算覆盖首槽，但仍将 `nhop` 增至 6。HPCC 首次更新只用 `NS_ASSERT` 检查容量，随后按 `ih.nhop` 写入发送端数组。断言关闭时这是越界写；之后的更新又因 `nhop > maxHop` 被跳过。两种情况都不能当作有效的 HPCC 遥测反馈。

**影响：** 当前默认跨 DC HPCC 结果不能信任；即使进程正常结束，也无法证明数组未被破坏。

**最小处理方向：** 让 INT 序列化空间、交换机追加逻辑、发送端逐跳状态容量都覆盖真实 6 跳，并明确拒绝超过容量的路径。不能通过丢弃一跳来规避，因为这会漏掉一个瓶颈。

容量修好后还要注意字段范围：当前队列长度只有 17 位、每单位 80 B，最大可表示约 10.49 MB；配置的交换机缓冲池是 50 MB。超过范围会截断。字节计数每 `128 MiB × INT_MULTI` 回绕；`GetBytesDelta()` 只能还原小于一个回绕周期的差值。高负载、多流情况下应确认同一流两次有效遥测样本间隔内的端口计数增量没有超过该范围。

### P1：TIMELY 高阈值低于当前跨域空载 RTT

位置：`config-longhaul-common.txt:20-24`；`longhaul-convergence.cc:735-740,979-985`；`seq-ts-header.cc:33-39`；`rdma-hw.cc:1212-1279`。

TIMELY 的时间戳在数据包生成时写入，随 ACK 回显，发送端据此计算 RTT；这条反馈链路是接通的。问题在于配置使用 `TLow=5 ms`、`THigh=10 ms`、`MinRtt=10 ms`。当前拓扑跨域单向传播时延为 `5 ms + 6 × 1.5 μs = 5.009 ms`，往返传播时延为 **10.018 ms**；这还未计入 NIC 处理、序列化及排队。因而空队列 RTT 已满足 `rtt > THigh`。

源码在该条件下直接计算乘性降速系数，不要求 RTT 梯度为正。跨域无拥塞流也会反复降速，直到路径变化或达到最低速率。论文中的 50/500 μs 是低时延数据中心实验参数，不适合不经校准地移植到约 10 ms 的跨域基准。

**最小处理方向：** 先测量当前模型的空载 RTT 分布，再按允许的排队时延确定 `TLow`/`THigh` 和梯度归一化基准；参数应适配跨域路径，而不是直接套用数据中心默认值。

### P1：FRP 入口和反馈路径没有接通

位置：`longhaul-convergence.cc:669-675,999-1008`；`switch-node.cc:77-85,510-512,560-570`；`rdma-hw.cc:1399-1435,1466-1470`。

`ResolveCcMode()` 只接受 `dcqcn`、`hpcc`、`timely`、`proposed`，所以 `--cc=frp` 会被拒绝。入口没有把交换机 `SwitchFeedbackEnabled` 设为 true，也没有调用 `StartPeriodicFeedbackMechanism()`；该功能默认关闭，因此即使单独补上模式映射，也不会产生周期反馈。

此外，FRP 发送端按 IP 第二段取 DC ID，而入口给所有主机分配 `11.0.0.(node_id+1)`，第二段始终为 0。跨 DC 流会被分类成同 DC 流，FRP 的 WAN/LAN 分支失效。反馈包穿过中间交换机时，`SwitchNode::GetOutDev()` 对 ICMP (`l3Prot==0x01`) 没有填入 ECMP hash 的第三个字，仍将整个 hash 输入读取；应在接通 FRP 前确认控制包路由采用已初始化的确定性 key。

**最小处理方向：** 模式选择、交换机反馈启动、DC 身份表示和 ICMP 转发必须一并接通；之后再验证反馈到达、限速及瓶颈变化恢复。当前不能把 FRP 结果与其余算法并列。

### DCQCN：反馈是 ACK 携带标志的简化模型

位置：`rdma-hw.cc:359-415,465-531,760-824`；`qbb-header.cc:47-49`。

接收端对 ECN 标记包设置 `FLAG_CNP`，并在 ACK 间隔为 1 时随 ACK 返回；发送端在 `ReceiveAck()` 中将该标志交给 `cnp_received_mlx()`。本入口没有交换机生成并发送独立 CNP 的接入路径，尽管底层保留了 `ReceiveCnp()`。因此它能跑出一种 DCQCN 风格的 ECN 反馈闭环，但反馈时机/抑制语义不是完整的 RoCEv2 DCQCN NIC 行为。

**结论：** 如果论文中的 DCQCN 是基准，应明确这是仿真器的 ACK 携带 CNP 简化变体，并通过“标记→发送端降速→标记消失→恢复”控制状态日志验收。若不验证这些动作，只凭 FCT 或仿真完成不能称作可信 DCQCN 对比。

## 会影响结果解释的统计问题

| 位置 | 问题 | 影响 |
|---|---|---|
| `SampleFlowRates():421-464`、`RecordCompletionSamples():270-285` | 发送速率由 `snd_nxt` 差分得到；丢包恢复会把 `snd_nxt` 回退到 `snd_una`，此时采样被跳过，且不计重传上网字节。第一次发现 QP 时直接以当前序号作基线，也会漏掉此前发送 | 该列既不是线速发送量，也不严格是单调的有效吞吐量；不能据此定位降速和判断链路利用率 |
| `SampleDciLink():470-503`、`switch-node.cc:237-242` | `tx_bps` 来自设备实际发送计数；`rx_bps` 使用的 `totalBytesRcvd` 是交换机把包交给该出口设备时增加的计数，并非 DCI 对端收到的字节 | CSV 的 `rx_bps` 名称不准确；它接近出口供给/入队量，不能直接当作 DCI 到达吞吐 |
| `SampleDciLink():486-495`、`get_pfc():309-315` | PFC 计数器是全网累计事件数，并同时包含发送和接收；每个采样点写进两个方向行 | 不是按 DCI 方向统计的 PFC 帧数，两个方向记录不可分别求和 |
| `qp_finish():288-300` | `standalone_fct` 用 `total_bytes * 8,000,000,000` 的整数乘法后再除以带宽 | `total_bytes` 超过约 2.3 GB 时乘法会溢出；且它未统一 NIC 延迟、PPP/L2、遥测开销等口径，不是严格的同算法独立流 FCT |
| `CalculateRoute():327-369`、`StartFlow():200-210`、`AddQueuePair():261-295` | 入口传入按路径瓶颈计算的 BDP；`AddQueuePair()` 在窗口非零时重新按 NIC 速率 × `baseRtt` 设置窗口 | 当前主机链路 100 Gbps、瓶颈也为 100 Gbps 时数值接近；换成低于 NIC 速率的瓶颈后，传入的路径 BDP 被覆盖，窗口会偏大。`pairRtt` 还是传播时延加单个 payload 序列化的近似值，不含配置的 NIC 延迟 |

**建议的最小测量口径：** 发送器控制目标速率、实际发送字节速率和接收有效字节速率分开；将 `rx_bps` 改为实际所代表的量或在文档中重新定义；把理论 FCT 下界与同算法独立流实测 FCT 分开。不要把这些不同口径的列合并解释为拥塞控制器行为。

## 简洁性和效率建议

先修正上述会改写算法含义或污染结果的问题，再做以下小整理；不需要重构框架或增加大量输入校验。

| 位置 | 建议 | 原因 |
|---|---|---|
| `CountDciEcn():317-324` | 对只读 packet 直接 `PeekHeader()`，去掉 `packet->Copy()` | 每个 DCI dequeue 少一次 packet 复制，不改变 ECN 计数定义 |
| `SampleDciLink():496` | 正常采样不逐行 `flush()`，仿真结束关闭文件时统一刷新 | 100 μs 采样周期下，重复强制写盘会增加 I/O；正常完成时关闭文件会刷新缓冲 |
| MMU 配置 `longhaul-convergence.cc:878-894` | 每个出口端口只调用一次 `ConfigEcn()`；速率/延迟读取移出 8 个优先级队列循环 | 当前每个端口相同的 ECN 阈值被重复写 8 次；清晰表达 ECN 按端口配置、headroom 按队列配置 |
| `SetRoutingEntries():398` | 用 `const auto&` 遍历 `j->second` | 避免复制每个 next-hop vector，且直接表达只读访问 |
| `FlowInput:135-145` | 把 `maxPacketCount` 改成 `size_bytes`，并为 node ID、端口、字节数使用对应宽度 | 现在字段名和实际单位相反，宽泛的 `uint64_t` 让单位/截断边界不明显 |
| `WriteRate():167-174` | 输出流已由启动阶段确认打开后，移除每次采样的 `is_open()` 分支 | 输出是必需项，重复判断没有恢复能力；失败已在初始化时报告 |

保留必要的文件读取、输出打开、流端点和拓扑索引错误处理即可。额外只需守住会直接造成结果错误或事件死循环的少数条件：流大小不超过当前 32 位序号上限、HPCC 遥测跳数有容量、采样周期和 payload 大小非零、当前链路速率在 ECN 阈值表中有配置。不需要为每个配置值建立通用校验层。

## 建议的最小验收顺序

1. 先把用于基准的流限制在 4 GiB 以下；确认收发有效字节数、完成状态和 FCT 能闭合。
2. 修正 HPCC 六跳 INT 容量，再核对队列长度和字节计数没有越过 INT 表示范围。
3. 为 TIMELY 测量空载跨域 RTT并校准阈值；无排队时控制器不能因为传播时延而持续降速。
4. 将 DCQCN 的实现边界写清，验证 ECN 反馈及控制器状态变化；将 FRP 的入口、周期反馈、DC 分类和转发路由连通后再单独验收。
5. 最后修正统计定义并做少量代码简化；保持正式实验的拓扑、负载、payload 和停止时间不因清理而改变。

## 参考资料

- [DCQCN: Congestion Control for Large-Scale RDMA Deployments（Microsoft Research）](https://www.microsoft.com/en-us/research/publication/congestion-control-for-large-scale-rdma-deployments/)
- [HPCC: High Precision Congestion Control（MIT 作者终稿）](https://dspace.mit.edu/entities/publication/2414a72f-069e-4df4-bc0e-b0db4474c959)
- [TIMELY: RTT-based Congestion Control for the Datacenter（SIGCOMM 2015 论文）](https://web.stanford.edu/class/cs244/papers/timely-sigcomm2015.pdf)
- [HPCC 作者仿真仓库中的 `rdma-hw.cc`](https://github.com/alibaba-edu/High-Precision-Congestion-Control/blob/master/simulation/src/point-to-point/model/rdma-hw.cc)

论文用于对照算法的反馈信号和设计边界；本报告的代码缺陷结论来自当前工作区源码，不把论文结果当作本程序的验证结果。
