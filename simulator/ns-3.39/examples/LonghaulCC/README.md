# LonghaulCC

入口为 `longhaul-convergence.cc`。`--cc=proposed` 使用 R5 逐流网关控制，主机仍运行 DCQCN。
其他模式为 `dcqcn/hpcc/timely/bifrost/frp/rocc`；通用 runner 支持前四种基线与 Proposed。
方案见 [R5 方案](../../../../docs/R5设计方案一.md)，实现依据见
[R5 代码设计](../../../../docs/R5代码设计与实现.md)。当前实现采用其文末 D1–D6 决策，D7–D9 暂缓。

## 运行

在 `simulator/ns-3.39` 下执行：

```bash
./ns3 build longhaul-convergence -j2
python3 examples/LonghaulCC/run-longhaul.py --algorithm proposed \
  --flow-files /absolute/path/to/finite-flow.txt --stop-times 0.1 \
  --purpose completion --output-root /absolute/path/to/new-results
python3 examples/LonghaulCC/analyze-longhaul.py --root /absolute/path/to/new-results
python3 examples/LonghaulCC/plot-longhaul.py --root /absolute/path/to/new-results --all-scenarios
```

若本机 `./ns3` 的 profile 与既有 CMake cache 不一致，可沿用已配置 cache：
`cmake --build cmake-cache --target longhaul-convergence -j2`。
直接运行模拟器时必须显式传入独立 `--config`，保存源码、二进制、共享库、配置、输入和命令的身份；
不能把旧 runner 构建记录当成这次构建的记录。

默认配置是 `config-longhaul.txt`；R5 控制参数在 `DciGatewayNode::Config`，主机参数在入口常量区。
`PROPOSED_OUTPUT` 指定 R5 文件前缀；runner 将其设为运行目录中的 `r5`。
runner 保存实际配置和输入副本及哈希，`--skip-build` 仅允许与构建记录完全一致的身份。
使用新的输出目录。有限流验收要求流全部完成；瞬态运行不能套用此要求。
Bifrost 仅支持 PG=3；`run-longhaul-all.py --algorithms dcqcn hpcc timely bifrost proposed` 可批量运行。

Proposed 固定使用 Fast CNP，不再提供 ECN 模式选择参数或配置键。
结果保存在 `proposed-fast-cnp` 目录，其他算法沿用正常 ECN。

本地/远端网关按每条流的方向确定：源 DC 的网关为本地网关，两侧均支持。
本地网关入站发现已有 CE 时立即反馈；WAN 出队命中正常 `ShouldSendCN()` 时也进入同一处理函数。
确认源 QP 支持并成功提交 Fast CNP 后，开启固定窗口 `[提交时间, 提交时间+cnpInterval)`，默认 50 μs。
窗口内同 QP 的拥塞信号复用通知：已有 CE 改为 ECT(0)，出口停止新增 CE；复用不延长窗口。
无有效窗口且提交失败、能力未确认或撤销时，保留已有 CE 和正常本地标记。
非 CE、DSCP、载荷、包长、其他头字段及校验和保持正确；远端角色、DC 内流和控制报文不清除 CE。
原生 ACK/NACK/CNP 仍正常转发，源端收到 Fast CNP 后由现有 DCQCN 调整速率。

缓存压力反馈共用发送入口和窗口，保留全局高低水位及逐 QP 响应等待。
**非紧急时仅反馈自身积压超过一帧、且最近采样窗口净积压增长超过一帧的 QP**；
一帧取配置的最大数据帧长，默认采样窗口 200 μs，用来滤除零贡献和单包采样误差。
全局进入压力状态不会直接惩罚所有流。紧急风险仍可向本轮有入站数据的 QP 反馈，保留能力核验和 50 μs 限频。
此筛选仅影响缓存压力 CNP，不阻止上游 CE 或本地 ECN 的直接反馈。

Fast CNP 使用 R5/v9 IPv4 控制报文，携带网关身份、原始接收端地址/目的 QPN/连接代号；
端侧通过 REGISTER_CONFIRM 声明支持，只接受自身 DC 网关的通知。它是仿真格式及上游 CE 清除扩展，
不是 IETF IPv6 线格式。成功提交不保证送达，提交、实际发送/失败和端侧接收分别计数。

`metadata.json`、`r5.final.json` 和 `runner-metadata.json` 记录固定策略与构建身份。
`fast_cnp_policy_version=4` 区分本次压力对象筛选；`near_cnp_policy` 和 `pressure_cnp_deadband_bytes` 记录规则与容限。
`gateways[].source_ingress_ce_packets/source_cleared_ce_packets` 分别记录匹配的入站 CE 和实际清除数；
`fast_cnp_ingress_handled_packets/ingress_coalesced_packets` 统计入站处理，
`fast_cnp_replaced_packets/coalesced_packets` 统计出口处理；submitted/tx/tx_failed 与端侧 received/rejected 核对通知路径。
`dci-link.csv` 的 `ecn_events` 只统计 CE。

在仓库根目录直接运行：

```bash
python3 simulator/ns-3.39/examples/LonghaulCC/run-longhaul-all.py
# 或只运行 Proposed
python3 simulator/ns-3.39/examples/LonghaulCC/run-longhaul.py --algorithm proposed
```

针对性回归（在 ns-3 根目录）：`./ns3 build longhaul-r5-ecn-test -j2`，然后
`./ns3 run 'longhaul-r5-ecn-test --output=/absolute/path/to/test-prefix'`。
通过真实 Qbb 接收、排队、出队及链路检查双向网关角色、CE/ECN 共用窗口、续开、跨 QP 隔离、
能力撤销、提交失败、报文完整性与源端 DCQCN；另以暂停不同 PG 制造积压，检查压力反馈对象与紧急保护。
有限流运行还需检查完成率、收发闭合、丢包和残留队列。正式比较应同时观察实际发送速率、接收 goodput、
FCT/完成率、队列、PFC、丢包及两类 CNP，吞吐或 CNP 数量单项不能说明优劣。

流文件第一行为数量，随后每行是：

```text
src dst pg dport size_bytes start_time_seconds
```

实验列表仅安排应用和统计。它不向网关提供连接列表、未来需求、起始时间、流大小或完成通知。
端点建链消耗真实传播和处理时间，所有算法均开启相同的模拟 QPN 格式及建链交换。

## 地址和连接身份

`longhaul-addressing.h` 仅根据拓扑划分 DC 和接入 Leaf。每 Leaf 一个 `/24`，每主机只有一个实际
非回环 IP。默认拓扑中，Leaf 32–35 对应 `10.1.1.0/24` 至 `10.1.4.0/24`；Leaf 73–76 对应
`10.2.1.0/24` 至 `10.2.4.0/24`。主机从 `.1` 向上分配，Leaf 接口从 `.254` 向下分配，
每 Leaf 最多 127 台主机；交换机互联使用 `10.3.0.0/16` 中的独立 `/30`。网关控制地址使用 `172.31/16`。
地址随节点身份和接入关系确定，不随链路行顺序改变。Qbb/RDMA 使用精确目的地址路由，不使用共享子网 ARP。

流键固定为 `(Src IP, Dst IP, Dest QPN)`。UDP 端口和 PG 是属性，不进入流键。
`RdmaHw` 分配 24 位 QPN，使用真实网络报文交换端点 QPN；数据、ACK 和 CNP 携带目的 QPN。
当前编码是四字节的版本化模拟字段（版本 1 + 24 位 QPN），不是完整 RoCE BTH 或完整 RDMA CM。
一个 RNIC 最多保留 4,096 个活动 TX/RX QP；其生命周期内 QPN 单调分配且不重用，耗尽明确报错或拒绝。
建链最多尝试 8 次，间隔 20 ms；错误和无效 QPN 计入终态汇总。

建链完成后，Proposed 的源端向本地 A 登记 `FlowKey → SrcQP/连接代际`。登记不创建数据控制状态。
A/B 在各自第一次收到数据时独立建表，两端本地 ID 可以不同；A 收到源端确认后才允许主动 CNP。

## 数据和控制路径

- `longhaul-r5.h`：网关公共路由与容量配置、源 QP 登记、元数据；不预注册实验流。
- `DciGatewayNode`：首包准入、B 观测及资源身份、运行时测时、A 全 RTT 预测/预算/本地反压。
- `QbbNetDevice`：每流四帧满桶、实际出队扣令牌、发送成功通知。内部兼容接口仍名为 InjectionGroup，但 R5 每个桶只属于一条流。
- `SwitchNode/BEgressQueue/SwitchMmu`：真实转发、PG/FIFO、ECN、共享缓存与 PFC。B 不新增逐流调度或限速。

A 的数据经过有界准入、逐流队列和令牌资格检查后发送。`frameBytes` 是本次运行最大数据报文的完整
序列化大小；桶容量为 `4 * frameBytes`，创建时满桶。修改速率先结算旧速率区间，零速率保留余额但
禁止发送，也不生成新令牌。空队列释放逻辑槽位，身份、桶和代际保留；再次入队不会再授予一桶额度。
令牌在出队扣除；实际发送成功才累加 TX，历史由周期累计快照生成，失败另计，不将目标速率或出队尝试当成已发数据。

新流首包入队后等下一次 5 µs 控制周期参与分配，候选速率取 A 物理出口容量，实际份额受共享出口与已知目的 NIC 容量约束；
不再使用 1% 冷启动速率。四帧额度按份额连续补充，不是发完后等待 B 授权。B 首包入队后主动反馈
流和资源状态，A 校验后直接进入反馈控制，不发送 QUERY。首份反馈丢失时由周期反馈继续更新。
当前协议为 wire v7；源 QP 登记和确认仍用于 near-CNP 关联，与网关查询不同。

B 分配不透明的队列资源 ID 和端口资源 ID。PG 只用于 B 内部真实队列关联，不是流分组键。
流状态和资源状态分开发送；资源能力不能逐流重复累加。A 对共享 B 队列、B 端口、A WAN 端口及已知
目的 NIC 物理上限联合做最大最小分配，逐流预测上限与本轮份额分开。目的 NIC 上限来自拓扑管理配置，
不等于测得的下游可用份额，也不能覆盖未知的共同下游瓶颈。

B 统计暂停区间、排空、完整报文出队和背景到达。独占资源使用物理容量；共享且持续积压时使用
`出队字节 / 未暂停时间`，曾排空时保留有效估计。反馈分别表达历史暂停时长、未暂停服务能力与当前
暂停状态；恢复后的未来服务不持续乘旧暂停折扣。B 保留 PG/FIFO，A 的逐流服务仅是按队列/近期输入
比例得到的近似，不是 B 承诺的公平份额。暂停变化后至多 5 µs 补采；到达 A 的时间仍包含网络延迟。

## 时间和预测

WAN 探测从真实发送开始到对端接收，扣除控制帧序列化后得到传播和接收处理基值。
逐包发送日志的到达估计使用自身帧长补回序列化；周期历史使用结束快照时的探测基值加最大数据帧串行化时间，并固定该区间映射。决策的 `d_f` 使用最大数据帧，`d_b` 使用实际控制帧。
测量前每向使用 5 ms 基值。源登记 ACK/确认往返测量本地时延，再加入源 RNIC 提供的反应定时器；
之后观察入口下降可更新等待估计，但这不是因果归因。仿真依赖共同时间基准，执行更新在当前事件原子生效。

A 每 5 µs 保存累计成功 TX 快照，按差分生成均匀到达区间；不会展开为逐包历史。反馈只保存观测，冻结预测在下一控制周期校验。每次 A 决策在时刻 `t` 冻结终点 `F = t + d_f + d_b`。从 B 的采样开始，先按成功发送历史补偿旧反馈；
再用当前积压、近期真实入口供给、候选份额、桶余额、暂停、链路占用和轮询位置推演未来发送。
只有 `t + d_b` 前的发送进入该未来计划，短尾包仍按自身序列化时间映射。历史按区间平均到达速率减服务、截零，未来段仍保留逐包计划；不能积攒空闲服务抵消后来的到达。周期平均会丢失区间内突发峰值，不能视为逐包预测的等价实现。最终计划同时记录端点和沿途峰值；共享池峰值在共同时间点
相加，样本无法对齐则标记未知。未来新流、未来暂停和下一轮控制动作不被假定为已知。

候选上限使用新动作生效前的历史队列与剩余可控区间计算；过去已经形成的不可避免风险、明确暂停、
陈旧状态或历史缺失可以停发。依 D4，本轮不增加共享启动/恢复许可，也不根据最终峰值增加反复收紧迭代。
记录峰值不等于保证多流同步满桶安全。冻结预测保留原值；后续同一采样时刻的反馈和实际历史用于校验。
D8 暂缓；周期历史按重叠区间裁剪，未来包在同纳秒采样边界仍保守包含到达，不宣称完成跨代累计核销或严格误差界。

本地 CNP 只由 A 的真实缓存压力触发，普通情况选择主要积压贡献流并等待本地/原生 CNP 的响应，排空时
停止追加；紧急情况通知当前贡献流。源 QP 关联未确认则不通知。CE 按上述 ECN 模式处理，原生 CNP/ACK-CNP 和 PFC 继续生效，
未改动主机 DCQCN 恢复参数。报文轮询不是任意包长下的严格字节公平。

## 有限资源与暂缓项

每网关最多 4,096 个数据身份和 4,096 个源登记；每 A WAN 出口 120 个可复用逻辑队列。历史按时间回收，保留边界前驱；窗口为反馈超时加最大初始/允许探测时延、最大帧串行化及一个控制周期，默认约 40 ms，每流约 8,000 条快照。历史条数不限制发送，待校验预测不延长历史。
每资源最多 1,024 个冻结校验点，每次未来推演最多 262,144 个包。B 每次最多报告 64 条流并轮换；
控制流量有 1% WAN 速率、64 KiB 桶及 64 KiB 待发队列检查。具体有效参数写入 metadata。

D7 等待缓存/MMU 协议、D8 跨代核销、D9 完整老化和重用暂缓。数据身份不会因队列空或应用完成而删除；
流表/队列不足明确拒绝并计数，不绕过整形。该策略不承诺无损承接任意规模短流。
关联消息有有限重试；失败时不错误绑定，源确认丢失时可保持禁止主动 CNP。固定单对直连网关，
不支持运行中迁移 PG/出口、自动重启会话或多入口共同资源协调；这些情形不能沿用旧记录悄悄控制。

## 输出与验收

公共输出包括发送 payload、接收 goodput、FCT、RTT、DCI 线速、PFC、队列与终态计数。
`metadata.json.addresses.csv` 保存实际接口 IP，`metadata.json.endpoints.csv` 保存运行时交换的 QPN。
终态区分唯一发送、实际 payload 发送、接收、连接拒绝、无效 QPN、准入丢弃及队列/MMU 残留。

R5 版本为 5、协议格式为 6、日志 schema 为 3：

| 文件 | 内容 |
|---|---|
| `r5.gateway-N.flows.csv` | A 的逐流候选上限、分配速率、令牌、供给、服务和实测时延 |
| `r5.gateway-N.packets.csv` | 独立 ADMIT/DEQUEUE/TX/TX_FAILED/DROP、身份、代际、完整报文计数 |
| `r5.gateway-N.resources.csv` | B 真实队列的服务、背景、暂停及资源 ID；其中 PG 是本地诊断属性 |
| `r5.gateway-N.predictions.csv` | FREEZE 全 RTT 终点、CHECKPOINT 周期检查点、VALIDATE 后验校验 |
| `r5.gateway-N.events.csv` | 创建、登记、关联、速率应用、CNP、实测时延、拒绝及资源/池峰值 |
| `r5.final.json`、`r5.summary.json` | 网关状态及公共终态汇总 |
| `r5-evidence.csv` | 分析器输出：冻结预测与以后真实 B 队列/峰值的比较，保留未知边界 |

分析和绘图按 schema 分支解释旧组日志与新逐流日志，不能混用身份和桶规则。
图中 target/service 是指令和估计，不是实际吞吐。PFC 应按暂停区间评价，不能按刷新行数计拥塞次数。
功能验收不证明吞吐优势。实际版本、验证范围和未完成机制见
[R5 实现与验证 §9](R5_IMPLEMENTATION_VALIDATION.md#r5-flow-20261016)。
