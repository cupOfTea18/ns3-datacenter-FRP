# LonghaulCC R4 代码详细设计与修改方案

日期：2026-10-08。依据：[R4 详细设计方案](LonghaulCC_R4_详细设计方案_20261008.md)。状态：依据 M0 讨论修订的后续代码修改规格；基础 R4 已有实现及实验，本次边界、许可规则和用户确认的乐观启动已实现；实际验证范围见 R4_IMPLEMENTATION_VALIDATION.md。

本文将算法方案映射到当前源码，给出建议类型、接口、事件顺序、协议和测试。代码片段是设计签名与伪代码，不代表仓库已存在这些接口。原方案是设计依据，不是自动执行代码修改或实验的授权。

2026-10-09 源码核对：本文保留设计规格与迁移背景，不作为完整的现行接口清单。
现行入口为 `longhaul-convergence.cc` → `longhaul-r4.h::Proposed::Setup()` → `DciGatewayNode`，
具体配置、调用顺序、已实现行为和测试证据范围见
[R4 实现与功能验证记录](../simulator/ns-3.39/examples/LonghaulCC/R4_IMPLEMENTATION_VALIDATION.md)。
实际 TxMode 还包含 `OptimisticStartup`；CE 包在 A 立即改为 ECT(0)，待反馈标志仅在实际近端 CNP 发送后消费。
主机 `ReceiveUdp` 的非零 ECN 反馈缺口仍存在；本次文档核对没有构建或执行回归。

## 1. 实施目标与范围

最终结构为 `DciGatewayNode` 持有逐流状态和逐端口协调状态；`QbbNetDevice` 执行逐队列整形、探测额度及底层暂停；普通 `SwitchNode` 继续转发、ECN、MMU/PFC。主机 DCQCN 不改，原生 CNP/ACK-CNP 正常返回主机，B 将边界后的反馈解释为远端域拥塞；A 消费源侧 CE、转成近端 CNP 后清除，并禁止 A/B 受控出口新增标记；B 不清已有 CE，下游普通交换机继续标记。

删除 R3 `Group` 及其历史、组队列、组限额和分片组快照依赖。保留流 ID/generation、物理端口和底层 PG，不用“每组一流”模拟最终结构。不引入通用控制器框架、插件层或多套策略继承体系。

完成标准：逐流状态、反馈、预测和执行形成闭环；端口容量和探测额度在真实出队路径生效；功能回归、消息异常测试及最小完成场景通过；研究性能另行验收，不能由功能测试代替。

## 2. 原始 R3→R4 迁移依据与修改入口

以下路径均相对 `simulator/ns-3.39/`。本节及原迁移清单保留初版设计背景，不是本次待做工作的当前状态；本次增量修改以第 15 节为准。

| 位置 | 原 R3 迁移时事实 | R4 处理 |
|---|---|---|
| `src/point-to-point/model/dci-gateway-node.h` | `Flow` 已有 in/tx、rate/budget/target；`Group` 持有 History/Snapshot/Assembly | 扩充 Flow，增加 PortState，删除 Group/Assembly |
| `dci-gateway-node.cc::RegisterFlow` | 逻辑队列从 qCnt 起分配；检查 IP/UDP/PG 匹配歧义 | 保留匹配与队列检查，删除 group 注册，建立 queue→flow 反向表 |
| `UpdateReceiver` | B 已跨 PG 按物理出口联合分配 | 保留算法，增加服务窗口、逐流目标与发送资格 |
| `UpdateSource` | 组间分配后再组内分配 | 每流产生上限，同出口一次分配 |
| `History` | 有固定宽度环形桶和非负逐桶回放 | 移至 A 流状态；加强启动、历史缺口和边界测试 |
| `R3Header` | 在 `dci-gateway-node.cc` 的匿名命名空间定义 | 原处替换 R4Header，无需改普通 qbb-header 格式 |
| `qbb-net-device.cc::SwitchSend` | QbbEnqueue 在队列 Enqueue 之前触发 | 新增成功入队 trace，R4 切换到新 trace，保留旧 trace 原语义 |
| `SetGroupShaperRate` | 更新一个队列后立即 DequeueAndTransmit | 新增端口批量应用接口，避免更新一半时开始发送 |
| `DequeueAndTransmit` | 根据暂停、令牌和 blocked 集合选择队列 | 加入探测额度及共享探测桶；出队后、trace 前扣减 |
| `GetQueuePauseTime` | 已能读取累计暂停时间 | 服务窗口复用累计差值，无需新增逐包暂停历史 |
| `BEgressQueue` | 128 个槽位、8 个物理优先级；支持逻辑队列 | 首版保留每出口最多 120 个受控逻辑队列；超限明确拒绝 |
| `longhaul-convergence.cc` | 内部解析 PROPOSED 配置，包含 longhaul-r3.h，通知 CompleteFlow | 原入口迁移 R4；没有独立 longhaul-config-parser.h |
| `longhaul-measurements.h` | 公共测量调用 Proposed::TracePath 等 | 保留命名空间和路径辅助函数，不破坏其他算法测量 |
| `run-longhaul.py` / `analyze-longhaul.py` | r3 文件前缀、队列模式和组聚合 | 新增显式 R4 schema/模式与逐流分析 |

当前 QbbEnqueue 不是“已成功入队”的严格接口；当前队列拒绝一般触发 abort，也不能因此让新计量契约依赖入队前事件。`BEgressQueue` 虽声明入队 trace，当前包装函数中的触发代码被注释，不直接假定可用。

## 3. 状态和所有权

### 3.1 数据类型原则

时间统一存 ns 的 `int64_t` 或 `Time`；计数用 `uint64_t`；控制计算用 byte/s 的有限非负 double。线上速率/队列目标向下取整为 uint64；日志 bps 只在输出时乘 8。累计计数不能用 double。

```cpp
enum class Role { Source, Receiver };
enum class Life { NotStarted, Active, Idle, Draining, Closed };
enum class ServiceSource { Paused, BusyObserved, BudgetFallback, BudgetOnly };
enum class QueueMode { History, Snapshot };
enum class TxMode { Normal, StartupProbe, RecoveryProbe, Stopped };

struct Registration {
    uint32_t id, generation, peer, port, pg, frameBytes;
    uint32_t sip, dip;
    uint16_t sport, dport;
    Role role;
    Time forwardDelay, feedbackRtt, start;
    double peerPortBytesPerSec;  // 由真实路径接线给出，仅用于远端报告范围校验
};
```

同一网关可同时拥有 Source/Receiver 流。`port` 始终是本节点执行出口，不能把 B 的出口编号直接用于 A。peer 的映射与容量上限由注册接线提供，报文不可信地自报端口不会改变注册关系。

`pg` 仅用于已有身份匹配、逻辑队列映射和查询发送资格。实际 flow_id 全运行唯一，generation 防止旧记录应用到新连接；首版仍拒绝运行中同线身份复用，不为动态回收扩展协议。

### 3.2 FlowState

采用一个 FlowState 内嵌小型 SourceState/ReceiverState，不引入虚基类。Receiver 的 History 不分配桶，避免双份大数组。

| 子状态 | 字段 |
|---|---|
| 公共 | Registration、logicalQueue、Life、admittedBytes、sentBytes、peakQueueBytes、lastArrivalNs、底层暂停状态 |
| 反馈 | alpha、reactionBytesPerSec、windowEvent、windowStartNs、lastCnpNs、decreaseCount、recoverySinceNs、previousTx |
| Receiver | budget、service、queueTarget、ServiceWindow、最近 DemandRecord、需求时间、lastBudgetChangeNs |
| Source | History、最近 StateRecord、haveState、fresh/historyCovered、predictedQueue、queueUsed、candidate、target、TxMode |
| 探测控制 | startupGranted、probeEpoch、lastGrantRxWatermark、grantSeq、已用探测累计量的读取基线 |
| 报告 | nextTxSeq、lastAcceptedSeq、lastReportNs、dirty；STATE/DEMAND 按角色各一个序列 |
| 近源反馈 | marking、lastNearCnpNs、上游 CE 待反馈状态和来源计数；两个来源共用限频，见第 15 节 |

队列权威计算为 `admittedBytes - sentBytes`；每周期对照 `GetLogicalBytes(logicalQueue)`。服务窗口的 `backlogged` 只在事件完成后由该差值更新。

### 3.3 PortState

```cpp
struct PortState {
    uint32_t port;
    std::vector<uint32_t> flows;  // 稳定 flow_id 顺序
    std::map<uint32_t,uint32_t> queueToFlow;
    uint64_t otherSentBytes = 0, previousOtherSentBytes = 0;
    Time lastCapacitySample;
    double physicalBytesPerSec = 0, availableBytesPerSec = 0;
    uint64_t targetQueueBytes = 0, sourceHighBytes = 0, sourceLowBytes = 0;
};
```

`otherSentBytes` 用累计计数差，容量分母使用实际两次采样时间差，不依赖总是恰好一个 control 周期。首周期使用 0 非受控流量估计并记录初始化标志。

探测令牌、单次字节额度的权威余额归 QbbNetDevice，控制器只读取设备统计和发出 grant。禁止双方各自递减一份余额。网关共享缓冲保护另保存 A/B 受控积压总和及高低水位状态；其作用域在日志中标明。

当前固定单对网关路径中，Source 的执行出口朝向 WAN，Receiver 的执行出口朝向本地网络。注册时断言同一物理出口的受控流角色一致；双向传输使用各自对应出口。若未来拓扑允许两种角色共用同一出口，必须先合并它们的候选上限再做一次分配，不能给两个角色各发一整份容量；首版对此不支持的映射明确拒绝。

## 4. 事件契约与设备接口

### 4.1 成功入队事件

给 QbbNetDevice 增加 `QbbAdmitted(packet, logicalQueue, priority)` trace。在 SwitchSend 中先完成分类和 `Enqueue`，成功后触发，随后才调用 DequeueAndTransmit。旧 QbbEnqueue 保持位置，避免静默改变公共测量。

```cpp
logical = classifier ? classifier(packet, priority) : priority;
bool accepted = m_queue->Enqueue(packet, logical);
NS_ABORT_MSG_IF(!accepted, "unexpected queue rejection after MMU admission");
m_traceAdmitted(packet, logical, priority);
DequeueAndTransmit();
```

当前 MMU 在 SwitchSend 前已计入占用，因此失败不能简单返回 false 留下脏计数。首版保留显式致命错误；正常准入丢弃仍在 SwitchNode MMU 检查处发生，不能转为“成功进入 B”。一般可恢复的设备入队失败及 MMU 回滚不属于此次扩展。

R4 OnAdmitted：先按旧积压状态累计 busy 时间，再增加 admittedBytes、设置新积压状态和 lastArrival，写事件日志。禁止在该回调中更新整形速率并重入发送，只置 dirty/needControl 标志。

### 4.2 实际出队事件

继续使用 QbbDequeue 获取数据真正离队、即将 TransmitStart 的事件。受控逻辑队列由 `GetLastLogicalQueue()` 和端口反向表识别；控制/普通报文计入 otherSentBytes。核验底层设备空闲情况下才会走该真实发送路径。

先完成设备令牌及探测扣减，再进入 R4 OnDequeue：累计旧状态 busy 时间，增加 sentBytes；A 记录 History，B 累计服务出队量；更新队列状态并写 event_order。观察回调不再调用 SetGroupShaperRate。

### 4.3 批量速率应用

新增简单接口，保留旧接口供未迁移测试/调用者使用，完成调用点迁移后再决定是否删除旧名称：

```cpp
struct QueueControl {
    uint32_t queue;
    double bytesPerSec;
    bool probe;
};
void ApplyQueueControls(const std::vector<QueueControl>& controls);
void ConfigureProbeBucket(double bytesPerSec, uint64_t burstBytes);
void GrantProbeBytes(uint32_t queue, uint64_t epoch, uint64_t bytes);
ProbeCounters GetProbeCounters(uint32_t queue) const;
```

ApplyQueueControls 必须先校验整批数据，再以同一个 Now 更新所有相关令牌、速率和模式，最后取消旧 shapeWake 并只调用一次 DequeueAndTransmit。总量检查按目标和容差完成；配置错误不得部分生效。

调用端校验该端口所有受控队列的完整目标向量之和不超过本周期 availableBytesPerSec；设备再次校验不超过物理容量。每周期包含所有已注册队列的更新（停发也显式写零），不把未出现在增量列表中的旧速率遗留在端口上。

同 epoch 的 GrantProbeBytes 幂等，不能在每个 tick 刷新额度；递增 epoch 才建立新 grant。切到 Normal 不扣探测余额；切到 Stopped 清空普通整形令牌，保留探测额度已用记录，避免模式切换重置字节预算。

### 4.4 探测执行

在现有 QueueShaper 增加 `probeMode, probeEpoch, probeRemainingBytes, probeSentBytes`。每设备一个 ProbeBucket。选队列前同时检查：PG 未暂停、流整形令牌足够、若为探测则额度可容纳完整队首帧、共享探测令牌足够。

选中后同时扣普通令牌、共享探测令牌和该流额度，再发 trace/TransmitStart。预算不是 packet payload，而是此路径真实 p->GetSize()。额度小于队首帧时阻塞直到新合法 grant，不发送半包。

等待唤醒按每条流所有可恢复令牌条件所需时间的最大值计算，然后在候选流之间取最小值。永久零速率、暂停或额度不足的队列不设置虚假定时器。新 grant、PFC Resume、批量目标变化、真实入队均能触发重新检查。避免“流令牌已够但共享桶不足”产生零延迟死循环。

普通数据不消耗探测桶；探测仍受最终端口目标分配。仅配置探测平均速率而没有实际字节扣减，不满足 R4 设计。

### 4.5 暂停观测

利用 GetQueuePauseTime(pg) 在控制采样边界做累计差，IsQueuePaused(pg) 得到当前状态。busy 统计包括暂停时长。首版最多下一控制 tick 更新控制器，底层 PFC 则立即停发；Resume 已有发送触发。无需更改原 PFC 报文或新增 PG 控制器。

## 5. 服务窗口的具体实现

首版约束 W_s 为 control 的整数倍，T_min 同样对齐；不保留无界逐包窗口。每流维护最近 K=W_s/control 个完成片段的环形数组：

```cpp
struct ServiceSlice { int64_t beginNs, endNs, busyNs, pauseNs; uint64_t txBytes; };
struct ServiceWindow {
    std::vector<ServiceSlice> slices;
    int64_t lastAccountNs = 0;
    bool backlogged = false;
    int64_t openBusyNs = 0;
    uint64_t openTxBytes = 0;
    Time previousPauseTotal;
    void AccountUntil(int64_t nowNs);
    void OnQueueChange(int64_t nowNs, bool nowBacklogged);
    void OnTransmit(int64_t nowNs, uint32_t bytes, bool stillBacklogged);
    void CloseSlice(int64_t nowNs, Time pauseTotal);
    ServiceObservation Read() const;
};
```

AccountUntil 按旧 backlogged 对经过时间累加。周期边界先累计到 Now，再封闭片段并开启新片段。控制 tick 与包事件同 ns 时按仿真执行顺序处理：边界之后的包进入新片段；window_begin/end 与样本序号记录这一口径。初始化窗口不补造 W_s 长度，记录实际已有时长。

服务估计使用完整片段 D/Tbusy；Tbusy<T_min 回退当前预算；当前暂停输出 0；其余取 min(budget,D/Tbusy)。若一个包瞬时入队出队使 busy=0，不能除零；实际 D 仍记录，估计走 fallback。暂停总时长不要求等于 busy 与暂停的交集，不从 Tbusy 中扣除。

每 tick 更新观测，即使本 tick 不发报告。所有提前报告也在 tick 中限频生成，不从入队回调直接采样半更新的状态。模式 `budget` 除暂停外直接使用 b_i；模式 `busy-observed` 使用以上规则。两种模式均保留 D/busy 原始观测。

## 6. Tick 调用顺序与核心函数

```cpp
void Tick() {
    const auto now = Simulator::Now();
    FinalizeServiceWindows(now);          // 所有 B 流
    SamplePortCapacities(now);            // 所有受控出口，累计差
    UpdateDemandAndLifecycle(now);
    UpdateBufferProtection(now);
    UpdateReceiverReactions(now);
    ComputeReceiverPortBudgets(now);      // budget + queueTarget + service
    ComputeSourceCandidates(now);        // fresh/history/probe，尚不发送
    ComputeSourcePortTargets(now);        // 所有流一次 water-fill
    ComputeNearSourceMarks(now);
    BuildControlReports(now);            // 从一致的已计算状态生成完整记录
    WriteDecisionSamples(now);           // 决策前累计计数和 ordinal
    CommitAllPortControls();              // 批量提交，可同步触发真实出队
    SendQueuedReportsAndNearCnps();       // 数据/控制均使用网络发送路径
    CheckQueueAccounting();
    ScheduleNextTick();
}
```

每个网关内先计算所有端口，再执行提交，防止一个端口同步出队改变尚未计算的另一个端口决策。报告队列样本取 BuildControlReports 时刻；随后的同 ns 出队属于样本之后，日志保留执行序。

接收控制包的回调只解析、校验、更新最近记录并置 dirty，下一个 tick 应用控制。迟滞不超过 control；不从 ReceiveRecord 递归进入整个 Tick。

### 6.1 主要函数清单

| 函数 | 输入/修改 | 关键契约 |
|---|---|---|
| RegisterFlow | Registration | 建立 data/feedback/queue 索引；一次配置队列 |
| OnAdmitted / OnDequeue | packet, port, queue | 成功入队/真实出队各计一次；无发送重入 |
| ObserveFeedback | CustomHeader | 保留原匹配；仅 B 设置反应事件 |
| UpdateReceiverReaction | Flow&, now | 不写其他流的反应状态 |
| AllocateCapped | caps, capacity | 空集/全零合法，非有限输入拒绝，稳定顺序 |
| ComputeReceiverPortBudgets | PortState& | 对所有活动 eligible 流联合分配，目标和<=端口目标 |
| EstimateService | Flow&, observation | 执行固定模式；标明来源 |
| BuildSourceCandidate | Flow&, now | 返回 v、TxMode、reason；不直接改设备 |
| ComputeSourcePortTargets | PortState& | 按有效需求和候选许可一次分配；瞬时空队列不清零 |
| PrepareProbeGrant | Flow&, remoteState | 单调 epoch、接收确认 watermark；不按每报告续发 |
| Build/ApplyStateRecord | Flow/record | 一条记录原子替换，不能字段半更新 |
| Build/ApplyDemandRecord | Flow/record | 关闭不抹除已有积压 |
| CompleteFlow | flow id | Source 标记 noMoreHostData；Receiver 通过 DEMAND 获知 |
| DoDispose | 全部状态 | 取消事件、断开 trace/分类器、释放历史/窗口 |

### 6.2 B 分配细节

需求为 `queue>0 || freshDemand.hasDemand || recentArrival`，recentArrival 窗口首版沿用 `2*forwardDelay+feedbackRtt`。Closed 不意味着立刻清队列；q>0 优先保持排空资格。暂停则 cap=0，但保留需求。

原保守方案以 r_probe 初始化（已实现的乐观启动替代规则见第 15 节），不要求先见到数据；收到有效 DEMAND 即可生成预算/STATE。恢复采用原有实际发送增长与近期到达条件，过期 demand 不会持续保留无限需求。空闲后重启保留身份，重置反应探测状态，但不重新发放“无 STATE 启动额度”。

目标 q_i*=Q_port*b_i/sum(b)；传输时向下取整，向下取整剩余字节可以不分配，确保总量不超界。无正预算时全零。B 的小反应上限不需要额外给每条流设一个无限累计的转发探测额度；总探测注入由 A 执行。

### 6.3 A 候选值与恢复优先级

修订要求：Closed 且队列空、未启动或本地暂停时停发；首次启动区分未获有效活动预算与明确远端不可发送，启动前零预算 STATE 不能跳过启动。进入正常阶段后，过期/缺历史或远端不可发送时停发，其余正常预测与公式。启动模式的具体限额与切换条件见第 15 节；活动零预算仍交给正常控制，不能简单将全部零预算改成满速。

正常值 `min(b,max(0,s-max(0,qUsed-qTarget)/tau))`。只有新鲜远端可发送、b>0、报告 q<=qTarget、预测/使用队列也不超目标，且 v 因零服务为零，才进入 RecoveryProbe。预测积压造成的零速率优先排空，不被探测绕过。

PrepareProbeGrant 在初次合法恢复时可以授予一次额度；随后仅在上次额度耗尽、有实际探测出队且更新 STATE 的 admittedBytes>上次授予时 watermark 时，授予下一 epoch。对同一次增加只消费一次：每次 grant 更新 watermark/seq。普通模式期间保留 probeEpoch 及已用计数，不能靠切换状态重置余额。

保守启动额度首次授权后永久消费；已进入正常活动阶段的流进入过期状态不重新启动。启动前的零预算记录不能用来判定启动完成。控制报文持续发送，使无数据状态也可恢复有效状态。

### 6.4 生命周期与尾部

当前 CompleteFlow 回调会遍历两个网关；R4 保留入口兼容，但仅 Source 角色直接接受主机完成通知。B 通过 DEMAND 的 CLOSED/no-more-host-data 标志获知，不绕过 WAN 读取全局完成状态。

首版不在运行中删除注册、历史或队列。Closed 标志禁止新启动/恢复探测；已有尾部数据继续按有效预算排空。收到 no-more-host-data 且本地无积压仍不释放线身份；结束时结合 A/B 累计计数检查在途和尾部。DEMAND 完成标志一旦成立不可倒退，旧序号不能重新激活。重复 CompleteFlow 幂等。

## 7. R4 控制协议

### 7.1 线格式

继续 IPv4 protocol=249、原控制地址/真实路由，所有整数 network byte order。Header 在 dci-gateway-node.cc 中实现，无新增普通 QBB 头字段。

公共头为 4 个 uint32，共 16 B：`version=4, kind, recordCount, recordBytes`。每包只含一种 kind；版本/长度先校验，再分配记录容器。STATE 固定 19 个 uint64，152 B；DEMAND 固定 8 个 uint64，64 B。

STATE 各 word 顺序：

| 索引 | 字段 | 单位/约束 |
|---|---|---|
| 0–3 | flow_id, generation, seq, sample_ns | ID 范围与注册一致；seq 严格递增 |
| 4–7 | queue_bytes, budget_Bps, service_Bps, queue_target_bytes | 非负；service<=budget；容量按注册远端端口检查 |
| 8–11 | admitted_bytes, sent_bytes, flags, service_source | sent<=admitted；差值等于 queue；枚举/bitmask 合法 |
| 12–15 | window_begin_ns, window_end_ns, busy_ns, window_tx_bytes | begin<=end<=sample；busy<=end-begin |
| 16–18 | window_pause_ns, last_budget_change_ns, sample_ordinal | pause<=窗口时长；change<=sample；ordinal 对应本节点事件日志 |

DEMAND word 顺序：`flow_id,generation,seq,sample_ns,queue_bytes,wire_sent_bytes,flags,sample_ordinal`。

flags 使用有名常量 `HAS_DEMAND, BACKLOG, NO_MORE_HOST_DATA, SENDABLE`，STATE/DEMAND 各定义允许掩码；BACKLOG 与 queue>0 一致。PAUSED 不再与 SENDABLE 双重表达。服务来源枚举单独字段，避免魔数散落。

recordCount<=floor((controlIpMtu-ipHeaderBytes-16)/recordBytes)。controlIpMtu 明确是不含 PPP 的 IP 包上限；序列化用真实 Ipv4Header 大小，线上统计另加 PPP。要求至少能容纳一条 STATE，不把 R3 的 PER_FRAGMENT=24 照搬。数据帧 burst 不限制最高优先级控制队列，控制 MTU 仍独立校验。

### 7.2 解析与应用

结构长度错误拒绝整包；先用除法检查 count 上限，防乘法溢出及未校验的大分配。语义非法记录逐条拒绝并计原因，不阻止同包其他独立合法流；同包重复 flow_id 拒绝后一个。

校验 peer 地址、角色对应 kind、generation、采样不在未来、不超时、seq 更新、累计计数不倒退，以及所有字段范围。旧版本报文直接拒绝。STATE 未校验前不能更新 b_i、累计确认或 freshness。

freshness 从采样时刻计算。相同采样时刻不同 seq 允许，前提符合计数和采样 ordinal 顺序；旧时刻即使序号更大也拒绝。seq/计数溢出明确终止运行，不回绕。

对跨包独立应用不建立 Assembly 或“整个端口收齐”条件。不同龄期预算超出远端实际份额是算法允许的暂态风险，分析必须记录；不能把 A 本端 sum(u)<=C_A 当成 B 容量也被严格保证。

### 7.3 报告调度

每流达到 period 或 dirty 且达到 reportMin 时入待发列表；按 peer/kind 分包，稳定 flow_id 顺序。每条记录的 sample_time、seq 和 sample_ordinal 属于生成时刻，不用真正出队时间覆盖。发送受网络排队时仍按旧采样时间判断有效性。

预算、sendable、目标队列、完成标志变化置 dirty。周期报告覆盖队列和服务变化，不为每次包事件立即发报告。无数据额度、暂停和过期均不关闭报告。

## 8. 历史重建及日志对齐

每个 Source Flow 初始化 History；保留 `[sample-forwardDelay, now]` 全区间。启动前无流数据按零处理，曾经覆盖但被覆盖的桶是缺历史，不返回零。

桶数按 `ceil((timeout+maxForwardDelay+2*bin)/bin)+1` 分配；实际用统一保留长度覆盖所有注册流，并记录桶数和内存。Snapshot 对照同样维护 History 并检查 Covers，只改变 qUsed。独立成本对照再允许禁用 History，并明确不同有效性语义，不混用于队列预测消融。

逐桶回放和边界均匀分摊承接 R3；加入整数 ns 边界校验、浮点非有限检查和非负截断。时间对齐考虑当前 trace 在序列化开始前触发；forwardDelay 包含传播、帧序列化及 B 接收处理延迟，固定包尺寸假设输出到元数据。

事件日志至少包括真实 A TX、B ADMIT、B TX，均有 `time_ns,node,event_order,flow,generation,bytes,in_total,tx_total,queue_bytes`。有真实 B ADMIT 后不必仅靠 A 时间平移猜测 B 输入；离线同时检查二者的固定延迟对应，识别丢弃或映射错误。

决策样本记录 B 报告 sample_ordinal 与累计计数，避免同 ns 简单 <= 截断错算。验证终点统一采用该纳秒全部 B 事件完成后的队列；如需模拟特定事件前状态则显式使用 ordinal，不混用。

## 9. 参数与兼容迁移

所有默认值只是工程起点，不代表调参结果。复用 R3 当前 period=200us、control=50us、bin=50us、reaction=50us、gamma=0.0625、recovery=200us、tau=20ms、timeout=20ms、utilization=0.98；其余旧反应速率参数显式输出。

| 配置 | 首版建议 | 迁移规则 |
|---|---|---|
| PROPOSED_VERSION | 4 | proposed 配置显式指定，不把老文件静默当 R4 |
| PROPOSED_RECONSTRUCT | 0/1 | 保留；runner 使用版本中性的 --queue-mode |
| PROPOSED_SERVICE_MODE | busy-observed | 枚举 budget/busy-observed |
| PROPOSED_SERVICE_WINDOW | period | 校验为 control 整数倍 |
| PROPOSED_SERVICE_MIN_BUSY | control | 0<T_min<=W_s |
| PROPOSED_PORT_QREF | 500000 B | 原 QREF 改名且改为端口总量；旧键在 R4 报明确迁移错误 |
| PROPOSED_SOURCE_PORT_HIGH/LOW | 250000/62500 B | 端口总量；按有需求流等分 |
| PROPOSED_PROBE_BYTES | 65536 B | 每次逐流 grant，不再与组绑定 |
| PROPOSED_PROBE_PORT_FRACTION | 0.05 | 探测桶速率占物理容量比例，与最终分配同时生效 |
| PROPOSED_PROBE_PORT_BURST | 65536 B | 所有探测共享，至少一最大帧 |
| PROPOSED_NEAR_CNP | 1 | 边界正常模式须启用；关闭需显式声明反馈消融 |
| PROPOSED_ECN_MODE | source-boundary | 必须显式设置；path 为旧反馈域对照 |
| PROPOSED_STARTUP_MODE | optimistic | probe 为保守对照；乐观启动无额外字节额度 |
| PROPOSED_SHAPER_ECN | 仅 path 模式 | 保留原 A/B 同时开关语义；boundary 模式拒绝此旧配置键 |
| PROPOSED_CONTROL_IP_MTU | 1500 B | 按已配置链路能力校验，不用常量决定条数 |

原 FALLBACK_FRACTION 在 R4 仅作共享缓冲应急容量比例，改名 PROPOSED_BUFFER_FALLBACK_FRACTION；反馈过期时是普通数据停发，不能沿用 R3 过期继续按旧组限额运行的语义。每个端口总目标/高水位求和必须符合网关配置的受控缓冲预算，失败直接拒绝配置。

注册完成后校验所有实际路径的 timeout、最大控制包、forwardDelay 和历史覆盖；不得只在 Configure 阶段检查还不存在的端口映射。

## 10. 逐文件修改清单

### 10.1 控制器与设备

1. `dci-gateway-node.h`：新增上述枚举/状态与函数声明，删除 Registration.group、Group、Snapshot/Assembly 与 m_groups。保留公开 Configure/RegisterFlow/Start/CompleteFlow/WriteMetadata 入口。
2. `dci-gateway-node.cc`：实现 R4Header、独立记录应用、服务窗口、逐流 History、Tick 分阶段计算、A 单层分配、探测授权和新日志；更新断言及异常原因。
3. `qbb-net-device.h/.cc`：新增 QbbAdmitted、批量应用、探测桶/额度及出队核验；保留原 PG/MMU 回调和控制队列优先路径。普通设备未配置新机制时行为保持原路径。
4. `broadcom-egress-queue.h/.cc`：首版原则上复用现有 PeekQueue、blocked 和 logical queue，不为 R4 新建调度器；只有实际接口缺失才补充。验证队列 0 控制优先级和普通队列不会被探测额度拦截。
5. `switch-node`、`rdma-hw`：不加入 R4 状态，不改 DCQCN 和普通 PFC；ECN 边界仅在 DciGatewayNode 的注册 Source 流实现，普通交换机标记保留；必要的现有功能回归必须继续通过。

### 10.2 接线、构建与配置

1. 新建 `examples/LonghaulCC/longhaul-r4.h`，承接 Proposed::Setup/WriteMetadata 和公共路径辅助函数；去掉 groups map/key 和 paths.csv group 列。
2. 修改 `longhaul-convergence.cc` include、配置解析和元数据版本；继续在同两个 DCI 创建 DciGatewayNode；CompleteFlow 保持角色限制。
3. 新建 `longhaul-r4-test.cc`，CMake 目标更新为 longhaul-r4-test；从 R3 测试逐项迁移公共回归，不通过空壳重命名绕过旧组断言。
4. 增加 `config-longhaul-r4.txt` 作为明确新版本配置，不直接把既有 R3 文件相同名字改成不同语义。runner 文档给出新的配置路径。
5. R4 验收前保留 R3 接线/测试源码和可复现快照，切换构建目标时不让旧 R3 测试因依赖已删除私有结构而参与编译。最终清理旧运行实现单独做，不批量删除历史论文、日志和脚本。

### 10.3 runner 与分析

`run-longhaul.py` 增加 `--queue-mode` 和 `--service-mode`，prefix 使用 r4；r3-only 选项用于 R4 时清晰报错，不默默忽略。同步批量入口 run-longhaul-all.py 的参数透传；元数据记录 version=4、模式、有效参数、输入路径与哈希、实际可执行文件身份。

现有 runner 的源码保存功能不能凭历史记忆认定存在。R4 运行另外保存源码版本及未提交差异、二进制哈希、输入副本和有效配置，供 R3/R4 比较；不把这项需求扩大成通用实验管理框架。

`analyze-longhaul.py` 按 proposed_version 分派 r3_evidence/r4_evidence；R4 使用 `(node,role,flow,generation)` 关联，A/B 关联再用已注册 peer，不能把 group 列伪造为 flow。预测误差使用第 8 节事件重建，不沿用最近后续周期样本冒充精确终点。

`plot-longhaul.py` 继续公共结果；新增必要逐流控制图。已有 run-r3-paper.py/plot-r3-paper.py 保持历史入口，不原地改成生成 R4 图。R4 专用论文脚本只有在明确需要时再建。

## 11. 日志和观测契约

输出采用 version=4/schema_version=1。建议分为：

- `r4.gateway-N.flows.csv`：每控制周期的 flow/generation/role/port、r/b/s、queue/target/predicted/used、v/u、actual cumulative counts、fresh/historyCovered、mode/reason、报告 seq/age、pause、服务窗口及来源。
- `r4.gateway-N.ports.csv`：物理/估计容量、受控目标和、实际受控/其他 TX、目标队列总量、探测令牌/字节和缓存保护。
- `r4.gateway-N.packets.csv`：ADMIT/TX 原始事件及 ordinal，供守恒与精确队列重建。
- `r4.gateway-N.events.csv`：STATE/DEMAND 收发与拒绝原因、探测 grant/exhaust、模式转换、原生/近源 CNP、共享保护。
- `metadata.json`：注册流路径、A/B 出口、底层 PG、固定时延、版本、全部有效参数、资源/逻辑队列上限。

控制包 TX/RX 使用独立计数；真实端口 otherTx 包含它们，不在容量统计再重复加一次。记录出队速率与 target 不同是合法状态，分析不得用 target 替代实际发送。

## 12. 回归测试明细

| 编号 | 用例与构造 | 断言 |
|---|---|---|
| U01 | 注册重复 ID、复用线身份、121 个受控队列 | 清晰拒绝，无静默合并 |
| U02 | caps=[20,80], C=100；caps=[100,100],C=100；空集/零 | [20,80]、[50,50]、空/零；总量合法 |
| U03 | 四条流任意端口列表顺序 | 不产生原“一流组对三流组”分配偏差 |
| U04 | 一流 CNP，另一流无 CNP | 仅前者 r/alpha 改变；预算变化由端口约束解释 |
| U05 | 空闲、即时入出队、持续积压、暂停、令牌阻塞 | busy/tx/暂停窗口精确、无除零、fallback 原因正确 |
| U06 | 报告/控制边界同 ns 先后事件 | 窗口计数唯一，sample ordinal 可重建 |
| U07 | 多队列批量降低速率 | 全部新值就位后才允许第一包发送，无部分更新泄漏 |
| U08 | P_probe 非帧整数倍，多流同时探测 | 每流不超过 grant，端口探测符合 bucket 界 |
| U09 | 同 seq/同 epoch重复、模式反复切换 | 不刷新探测量；只有新确认和合法新 grant 可续 |
| U10 | 初始无数据/无 STATE，A DEMAND 先到 B | B 主动给预算，零数据不造成协议死锁 |
| U11 | 暂停→恢复、过期→新报告、历史覆盖恢复 | 保守停发后可恢复；控制消息持续 |
| U12 | 预测高积压、service=0、budget>0 | 不用恢复探测绕过排空限制 |
| U13 | 真实发送突发、中途排空、部分桶、历史过期 | 对已知人工轨迹得到预期重建；缺历史显式无效 |
| U14 | R4Header round-trip、短包、巨 count、错版本、乱序/错代次 | 无越界/大分配，合法记录独立应用 |
| U15 | 一个控制包丢失，另一包含其他流 | 其他流更新，不等端口“收齐” |
| U16 | A/B 同时承担两个传输方向 | 角色、端口和 peer 不串流 |
| U17 | CompleteFlow 时网关还有尾部 | 不丢尾部、不释放身份，最终字节闭合 |
| U18 | 成功入队 trace 与 MMU 拒绝 | 拒绝包不进入 admitted 计数；真实队列与 in-tx 相等 |
| U19 | PFC 续期/自动恢复、控制队列优先、未启用 R4 的普通转发 | 公共行为回归通过 |
| U20 | qTarget 分配与源端等分水位，活动集合改变 | 目标和不随流数膨胀，高低阈值合法 |

分析测试用手工小事件表，验证同 ns 边界、代次隔离、累计字节、模式识别和 endpoint 队列；不使用待测函数生成期望值。消息随机字节测试可补充解析健壮性，但不代替上述具体边界。

## 13. 实施顺序和阶段验收

| 阶段 | 可审查产物 | 进入下一阶段条件 |
|---|---|---|
| P0 保存基线 | 当前源码差异、配置与 R3 运行版本出处 | 能区分历史 R3、当前源码和后续 R4 |
| P1 设备执行接口 | 成功入队 trace、批量控制、探测执行 | U07–09/U18–19 通过；非 proposed 行为未改变 |
| P2 状态与协议 | Flow/PortState、R4Header、接线与配置 | U01/U14–16，通过编译；无 Group 运行状态 |
| P3 基础控制 | B 反应、A snapshot、端口分配、恢复/结束 | 分配与状态测试通过；小流完成/字节守恒 |
| P4 历史与服务 | History、ServiceWindow、模式对照 | 人工轨迹和事件对齐测试通过 |
| P5 工具与机制验收 | runner/分析/日志/最小场景 | 有效配置和出处完整；控制动作到结果可追踪 |

测试目标需随依赖先建可编译骨架，P1 设备测试可先放独立测试方法；不强求所有中间代码均同时运行完整 R4。每阶段提交范围应明确，不把性能调参混入设备正确性修复。

后续实施时的预期命令（本轮未执行）：

```bash
cd /home/shemuping/newCode/ns3-FRP/simulator/ns-3.39
./ns3 build longhaul-convergence longhaul-r4-test -j2
./ns3 run longhaul-r4-test --no-build
python3 -m py_compile examples/LonghaulCC/run-longhaul.py examples/LonghaulCC/analyze-longhaul.py
python3 examples/LonghaulCC/test-longhaul-analysis.py
```

新增 R4 分析测试文件加入对应测试命令。性能场景另按研究方案执行：共享出口、异质下游、加入退出、暂停恢复；hist/snapshot、budget/busy-observed、近源 CNP 和整形 ECN 分别控制变量。不能一次同时改所有开关后归因。

## 14. 实施前必须保留的风险与设计澄清

1. 忙时服务模型仍可能高估/低估未来服务；实现正确不等于模型有效，不擅自加入未讨论的复杂预测器。
2. 快照逐流独立更新允许混合龄期预算，首版不提供 B 端口带宽预留保证。
3. 逐流逻辑队列上限、O(N*历史桶数) 内存、每流报告成本需要实测，不能以允许仿真实现推导硬件可部署。
4. 保守 probe 启动/恢复具体化为设备持有 byte grant，避免仅按周期计算探测速率而越额；默认 optimistic 不使用额外启动字节额度。“暂停观测”使用累计暂停接口和 tick 轮询。
5. 此前建议的版本 4 线格式、服务窗口对齐和配置已在现行 R4 中实现；具体数值以配置为准，设计伪代码和验收清单不等于实际接口或全部已通过测试。不能回写历史 R3 结果作为现状。

本轮已同步修改控制器、接线配置和回归测试，并执行构建及有界验证；未修改主机算法。执行范围和结果见 R4_IMPLEMENTATION_VALIDATION.md。


## 15. M0 排查后的增量修改规格（2026-10-08）

本节取代旧版“不隔离 ECN”和“空队列撤销份额”的规则；前文 R3 迁移工作不需要重做。以下已实施，验证结果另行记录，不回写旧实验结论。

### 15.1 A 的 ECN 边界

- `dci-gateway-node.h/.cc`：为 Source 流增加上游 CE 待反馈状态及必要来源计数。接收受控 UDP 数据时核验注册身份、Source 角色及对应 WAN 出口，读取 CE 后置待反馈；不能依赖 A 已有积压。
- 在正常转发前将该 CE 改成 ECT(0)，保持 DSCP，并同步 `Packet` 内 IPv4 头和本次转发使用的 `CustomHeader`；校验头长/校验和处理。不修改非 CE、非受控流、控制包或错误方向的包。原 ECT 值无法从 CE 恢复，因此明确使用 ECT(0)，不是宣称恢复原值。
- 修改 `ShouldMarkEcn`：source-boundary 模式中，匹配注册流及其受控出口即禁止新增标记，覆盖 Source/Receiver 两种角色；未匹配流或其他出口继续普通 ECN。`ConsumeSourceCe` 仍仅对 Source 生效，B 保留已有 CE。path 模式沿用旧 shaperEcn 语义。
- `UpdateSource`/`SendNearCnp`：触发条件为上游 CE 待反馈或 A 水位标记，两个原因合并成一次逐流反馈，不要求 CE 分支队列非空。沿用反馈限频；当前实现只在实际近端 CNP 发出后清除 pending，原生反馈抑制不消费 pending。记录 upstream_ce/local_queue/both 来源，保留实际发送与合并/抑制原因。
- `longhaul-r4.h`、配置解析及元数据：替换旧 `path ECN; no boundary isolation` 描述，明确 A 边界是否启用、CE 转近端、清除、新标记禁用与 B 标记状态。必须显式设置 `PROPOSED_ECN_MODE=source-boundary|path`；旧 `PROPOSED_SHAPER_ECN` 仅允许与显式 path 模式一起使用，边界模式禁止该旧键。正常配置拒绝清 CE 却关闭近端反馈的组合，实验消融必须明确声明。
- 不变更数据身份、包长度或 MMU 计数；边界按流角色识别，支持同节点承担双向 A/B。A 的真实出口竞争也由近端反馈、端口分配及 PFC 处理，不宣称从共享物理队列识别了整形等待的具体原因。

### 15.2 空队列保留许可

`UpdateSource` 端口分配删除 `f.Queue() ? f.source.candidate : 0` 对瞬时队列的门控，使用已经过需求/生命周期、暂停和报告有效性判断的候选值。仍对所有目标联合分配，保证总目标不超容量。

`ApplyQueueControls` 保持合法目标；不要因为队列变空清空速率或每次到包重新发额度。现有入队后的 `DequeueAndTransmit` 按保留的许可调度，无需在观察 trace 内重入控制。令牌仍有有限 burst，空闲时不得无限积累；暂停、令牌不足、真实零候选仍会阻塞。空队列时没有实际发送，不写虚假历史。该方案暂不增加事件驱动的份额借用机制。

### 15.3 用户确认并实现的乐观启动

用户选择无额外启动字节额度。新增 `PROPOSED_STARTUP_MODE=optimistic|probe`，默认 optimistic；`TxMode::OptimisticStartup=4` 追加枚举，不改变原有 0–3 值。

`SourceState::activeStateReceived` 只在接收通过校验、sample>=start 且 HAS_DEMAND 的 STATE 后置真，一旦置真不回退。启动前/无需求零预算报告不接管；活动零预算是有效接管，不能被启动绕过。

未接管期间，A 候选为 min(本地物理容量, 注册的 B 出口容量)，接受统一端口分配和缓冲保护；CommitControls 不启用探测桶，也不授予 probe 字节额度。普通整形令牌仍有有限 burst。新鲜明确远端暂停先于启动处理；从 start 起达到既有 STATE_TIMEOUT 仍未接管则停发，控制报告继续。

B 用 ReceiverState::started 区分首次活动与重启。optimistic 首次活动以本地物理容量初始化反应上限、不应用 probing 小速率限制，实际预算仍由 utilization、其他流量和多流分配决定；后续重启继续保守探测，不重新线速初始化。probe 模式保留小速率与一次 grant，但启动条件同样不能只看 haveState。正常后的失效、缺历史、预测排空和恢复探测规则保留。

### 15.4 增量回归与执行顺序

| 编号 | 构造 | 验收 |
|---|---|---|
| U21 | A 收到 CE；A 队列空或低于水位 | 近端反馈可触发；跨域包为 ECT(0)，DSCP 不变，缓存头一致 |
| U22 | 连续 CE、水位同时触发、限频期间再到 CE | 合并为同一逐流反馈；待反馈不因限频/空队列丢失，来源可核对 |
| U23 | A/B 整形积压、B 已有 CE、非受控物理队列及下游拥塞 | 真实出队不产生 A/B 新 CE；B 透传已有 CE；普通队列仍标记，下游反馈仍触发 B 反应 |
| U24 | 双向流、非受控流、非 CE、Not-ECT | 只改对应 Source 的受控 CE，其他语义不变 |
| U25 | 正常活动流采样时队列空，tick 之间到包 | 保留目标；有令牌、无暂停时不等下一 tick，实际 TX/历史正确 |
| U26 | U25 加暂停、令牌不足、完成或报告失效 | 保留许可不绕过真实发送限制，有限 burst 与端口总量成立 |
| U27 | 启动前零预算 STATE；多流同时启动 | 不错误跳过启动；验证容量上界、活动零预算接管、超时及重启不续发满速；实测瞬态积压 |

先实现并独立验证空队列许可、ECN 边界，再实现用户确认的无额外字节额度启动。分别保留独立对照与组合结果，M0 先检查首包等待、各阶段实际吞吐、反馈来源、FCT、收发闭合、队列/PFC/丢弃，再扩展多流和异质下游。先不修改主机恢复参数或高低水位；修订前约 27/87 ms 的结果只是诊断基线，不是新设计的效果承诺。


### 15.5 B 受控整形出口 ECN 修订

`ShouldMarkEcn` 在 source-boundary 模式下对匹配受控出口的 A/B 流统一返回 false；不改普通 SwitchNode、MMU、PFC、主机算法或控制律。B 队列通过既有 STATE→A 排空项处理，避免 B 自身标记经接收端反馈回来继续压低 B 服务预算；来自下游的反馈继续保留。

`longhaul-r4.h` 输出 `ecn_policy_revision=2`，receiver_shaper_ecn 与 source_shaper_ecn 在 boundary 模式均为 false；feedback_scope 明确 B 保留已有 CE。旧 `shaper_ecn` 是 path 模式兼容参数，不能将其 true 解读为 boundary 下 B 仍标记。boundary 对旧键及禁用近端反馈的配置拒绝规则保持不变。

回归使用真实设备出队：A/B 受控队列超过 ECN 阈值也不新增 CE；已有 CE 实际穿过 B 后仍存在；非受控物理队列仍产生 CE；B ObserveFeedback 仍识别下游 CNP。真实拓扑复测沿用上一轮四个输入与 180 ms 停止时间、seed/run=1，独立保存结果；不同时修改启动参数掩盖结果。

待确认的兼容修正：ReceiveUdp 的拥塞计数及 ACK/NACK CNP 标志应仅由 CE 触发，而非所有非零 ECN。当前 ECT(0) 改写会被误报为拥塞，需经范围确认后修改 rdma-hw.cc 并新增四种 ECN 值的接收反馈回归。本轮只交付 B 标记修订，未应用此兼容修改。
