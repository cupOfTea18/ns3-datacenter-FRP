# R4 实现与功能验证记录（2026-10-08）

实现依据：[R4 代码修改方案](../../../../docs/LonghaulCC_R4_代码详细设计与修改方案_20261008.md)。
本记录描述实现及实际执行过的检查，不代表性能、选择性收益或持续饱和稳定性已经成立。

## 当前源码核对（2026-10-09）

本节依据工作区的 `longhaul-r4.h`、`longhaul-r4-test.cc`、`longhaul-convergence.cc`、
`dci-gateway-node.{h,cc}`、设备执行路径与 runner。工作区已有未提交改动；本次只更新文档，
没有构建、运行回归或实验。下方 2026-10-08 的通过记录和实验数字属于已有证据，
不自动证明当前源码与历史二进制逐字对应。

### 入口与调用关系

- 主程序包含 `longhaul-r4.h`，`--cc=proposed` 使用 R4，保留主机 DCQCN（模式 1）。
  C++ CLI 为 `--conf`、`--cc`、`--flow-file`、`--stop-time`、`--seed`、`--run`；
  读取配置后重解析 CLI，由显式命令行值覆盖配置。
- runner 默认读取 `config-longhaul-r4.txt`；直接 C++ 省略 `--conf` 时仍读取 `config-longhaul.txt`。
  当前 Proposed 必须显式选择版本 4，不隐式迁移旧 R3 配置。
- 入口创建两个 `DciGatewayNode`，普通 Leaf/Spine 为 `SwitchNode`；流调度注册后调用
  `Proposed::Setup()`，再写元数据、启动公共测量。接线保存配置/注册信息，运行时状态属于网关。
- `TracePath()` 复用 `LookupOutputPort()`，识别 WAN 两端、B 向内出口，并检查 ACK-CNP 与独立 CNP 回程经过 B。
  跨域流 ID 为输入序号加一、代次默认为 1；本地流不注册 R4。线身份为注册 IP/UDP/PG，不是硬件双方 QPN。
- `Tick()` 顺序：关闭 B 服务窗口 → 采样端口 → 更新需求 → 计算 B 控制 → 计算 A 控制 →
  生成报告 → 记录决策 → 批量提交设备控制 → 发报告与近端 CNP → 队列守恒检查。
  决策采样早于设备提交，target 不等于同一时刻的实际吞吐。
- 完成回调向两端调用 `CompleteFlow()`，函数只关闭 Source 状态；B 通过 DEMAND 接收关闭标志，尾部继续排空。
  停止汇总由 `LonghaulMeasurements::Finish()` 生成，包含 R4 同口径副本。

### 已实现机制

| 对象 | 源码行为 |
|---|---|
| 端口资源 | 无运行时 Group/Assembly；容量估计为 `max(0, utilization×物理速率−上个采样间隔非受控实际出队速率)`；A/B 各端口对所有流一次带上限等权分配，跨 PG 共享容量 |
| B 反应 | 反应窗口合并 CNP，更新 alpha，有事件才乘法降低上限；恢复需要实际发送增长、近期到达、无近期 CNP 与无暂停 |
| B 目标队列 | 按各流预算比例分配端口 qref 并向下取整；暂停流预算为零；目标不是缓存预留 |
| B 服务 | busy-observed 为 `min(预算, 窗口出队字节/积压墙钟时间)`；积压时间包含暂停/令牌/调度等待，不扣暂停；当前暂停输出零，忙时不足回退预算；budget 模式直接用预算 |
| A 历史 | 真实 WAN TX 按流分桶，从 `sample−df` 回放到 now，对应 B 模型终点 `now+df`；逐桶非负截断，边界桶近似分摊，服务保持快照值 |
| A 正常控制 | 候选为 `min(budget, max(0, service−max(0, queue_used−queue_target)/tau))`，再参与端口分配；history 使用预测、snapshot 使用快照，两者均维护、计算与校验历史 |
| 许可与失效 | 活动流瞬时空队列保留合法许可；接管后过期/缺历史/暂停停止普通注入，但继续报告 |
| optimistic | 首次启动候选为 A/B 出口物理容量较小值，受本端分配约束，无额外启动字节额度；第一份有效活动 STATE（包括零预算）接管后不重授；新鲜不可发送状态优先阻止启动；start+timeout 未接管则停发 |
| probe/恢复 | probe 启动额度只授一次；恢复要求新鲜可发送、正预算、零服务、快照/使用/预测队列均不高于目标且未关闭；首份恢复额度单独授予，后续要求额度不足一帧、实际发送、新序号及 B 累计接收推进 |
| 设备执行 | 成功入队计 ADMIT；完整端口控制批量应用；取出可发送报文后、QbbDequeue 通知与链路发送前扣普通令牌，探测模式另扣逐流额度和共享端口探测桶；重复 epoch 不刷新额度；optimistic/normal 不使用探测额度 |
| ECN 边界 | A 对匹配 Source 身份及注册出口的 CE 置 pending 并立即改为 ECT(0)，保留 DSCP/长度/解析字段，按全局设置重算校验和；清 CE 先于近端 CNP 发送；pending 只在实际近端 CNP 发出时消费，原生/近端限频不会丢失 pending |
| ECN 出口 | source-boundary 的 A/B 注册受控出口不新增 CE，B 保留已有 CE；其他流/出口及普通交换机沿用 ECN；A 观察原生反馈只记录和抑制近端重复反馈，不执行 B 的乘法反应 |
| 协议 | IP 协议号 249、version 4、16 B 公共头、STATE 152 B/DEMAND 64 B；按 peer/kind 与 IP MTU 装包，单记录不跨包，独立校验应用；结构错误整包拒绝，来源/身份/代次/序号/时间/计数等语义错误逐条拒绝 |
| 资源保护 | 每出口最多 120 流，同一受控出口不混合 Source/Receiver；各 B 端口 qref 与 A 端口 sourceHigh 合计须小于共享缓存 80%；网关 A/B 积压分别以 80%/50% 触发/退出保护，缩减端口容量，B 排空也受影响 |

### 配置与迁移

以下为 `config-longhaul-r4.txt` 的实现起点，不是性能标定值。时间为秒、速率为 byte/s、队列为 wire byte；
逐流 `_bps` 日志转为 bit/s，端口 `_Bps` 为 byte/s。

| 配置 | 当前值或要求 |
|---|---|
| 版本与 ECN | 必须显式 `PROPOSED_VERSION 4`、`PROPOSED_ECN_MODE source-boundary|path`；默认配置为 source-boundary |
| 启动与近端 CNP | STARTUP_MODE optimistic，NEAR_CNP 1；边界模式必须启用近端 CNP |
| 队列/服务 | RECONSTRUCT 1，SERVICE_MODE busy-observed；runner 用 `--queue-mode`、`--service-mode` 覆盖 |
| 周期 | REPORT_PERIOD 200 μs；CONTROL_PERIOD/BIN_WIDTH/REACTION_WINDOW/CNP_INTERVAL/REPORT_MIN_INTERVAL 均 50 μs；RECOVERY_PERIOD 200 μs |
| 服务窗口 | SERVICE_WINDOW 200 μs、SERVICE_MIN_BUSY 50 μs；省略分别用报告/控制周期；要求控制周期整数倍 |
| 排空与水位 | PORT_QREF 500000 B、TAU 0.02 s；SOURCE_PORT_HIGH/LOW 250000/62500 B；不随流数复制总量 |
| 反应与速率 | GAMMA 0.0625；PROBE_RATE 125000000 B/s、RATE_INCREASE 625000000 B/s、UTILIZATION 0.98 |
| 失效/保护 | STATE_TIMEOUT 0.02 s，须大于 df+3×报告周期；BUFFER_FALLBACK_FRACTION 0.05 |
| 探测/报文 | PROBE_BYTES/PROBE_PORT_BURST 65536 B，PROBE_PORT_FRACTION 0.05，CONTROL_IP_MTU 1500 |

表中省略了共同前缀 `PROPOSED_`。旧 QREF、SOURCE_HIGH/LOW、FALLBACK_FRACTION 会登记为迁移错误，
对应新键为 PORT_QREF、SOURCE_PORT_HIGH/LOW、BUFFER_FALLBACK_FRACTION。
旧 SHAPER_ECN 只允许显式 path 模式：1 保留 A/B 新标记，0 同时关闭且不清已有 CE；boundary 必须删除该键。
MTU 1500、IPv4 头 20 B 时，每包最多 9 条 STATE 或 22 条 DEMAND；PPP 头不计入 IP MTU。

### 输出、测试与证据边界

runner 前缀为 r4，输出 gateway flows/ports/events/packets CSV、paths CSV、公共 metadata 和 summary。
直接 C++ 未设置 PROPOSED_OUTPUT 时，以 `SUMMARY_META_FILE + ".control.csv"` 为前缀。
R4 paths 只记注册跨域流，完整数据/ACK 路径及本地流由公共测量记录。
runner 完成验收核对 FCT 数量/身份及唯一发送、接收 payload，R4 另查两端 wire 闭合；
丢弃、重传和队列/MMU 残留仍需独立审核，completion_valid 不等于无损或性能通过。

`longhaul-r4-test.cc` 为独立确定性控制器/设备回归，CMake 登记为 example 目标。
无参数执行包含 PFC 续期、设备/准入、边界、许可/启动、分配、服务、历史、消息与生命周期检查。
11 种拒绝输入需另用独立进程执行：duplicate、identity、queues、mixed、batch、capacity、timeout、
boundary-no-near、boundary-no-ecn、ecn-mode、startup-mode。无参数运行不自动执行这些 abort 分支。
下方历史记录中的测试通过不表示本次重跑，也不表示主机 ECN 语义、随机控制丢包压力、长期饱和或硬件成本已验证。

### 当前信息缺口

- `RdmaHw::ReceiveUdp` 仍以 `ecnbits != 0` 计拥塞、以 `if (ecnbits)` 设置 ACK-CNP；
  A 改写的 ECT(0) 仍可能产生远端反馈。网关边界已实现，端到端隔离未成立，回归没有接收端仅 CE 反馈修正。
- 固定注册、单对 DCI、固定路由/帧时延和仿真同步时钟；多网卡主机、线身份复用、同出口混合角色明确拒绝。
- 不同龄期 B 预算不是容量预留；历史非受控出队和常值服务不保证未来份额。snapshot 不是零历史成本对照。
- optimistic 在途量可能大于队列目标；应急保护同时限制 B 预算与排空，不能写成仅限制 A 注入。
- 既有结果属于各自归档源码与配置，当前工作区与历史归档的完整对应关系尚未核验；参数及普遍性能优势仍待验证。

## B 受控整形出口禁用新 ECN（2026-10-08 后续修订）

本节为最新修订；下方保留此前仅关闭 A 标记的失败记录，不能作为当前结果。

- `source-boundary` 模式中，A/B 注册受控出口均不新增 CE；B 不清除已有 CE，非受控流、其他出口和下游普通交换机保持 ECN。path 模式兼容行为不变。
- B 积压仍通过 STATE 与 A 的排空控制处理；下游反馈、PFC、端口竞争及既有缓冲保护仍可限制 B。没有修改启动策略、利用率、主机恢复参数或水位。
- 元数据增加 `ecn_policy_revision=2`，A/B 的实际新标记策略均为 false，避免与旧 source-boundary 结果混淆。
- 构建、控制器/设备回归、Python 分析 8 项及 diff 空白检查通过。新增真实出队测试覆盖 A/B 不新标记、B 已有 CE 透传、普通物理队列仍标记和 B 仍接受下游反馈。

仅关闭 B 新标记的单因素结果：`results/r4-b-ecn-validation-20261008-ug78j2pu/runs/`。四个输入、seed/run=1、180 ms 停止时间与上一轮相同，runner 保留有效配置和源码/构建身份。

| 场景 | 完成 | FCT（ms） | admission drops |
|---|---:|---|---:|
| M0 200 MB | 1/1 | 30.434831 | 0 |
| 8 条同时启动，各 20 MB | 8/8 | 20.204844–33.335682 | 0 |
| 双向各 1 MB | 2/2 | 两条均 10.314479 | 0 |
| 两条共享 B 出口跨域流 + 本地竞争 | 3/3 | 跨域 15.388477、15.399512；本地 18.644766 | 0 |

四个场景主机 TX/RX 等于流大小，A/B 入队出队闭合，停止时交换机队列为零；逐采样守恒及端口容量上界通过。M0 B 的原生拥塞反馈和降预算事件均为零，B 队列峰值 666,528 B；A 仍有 18 次本地队列近端 CNP，FCT 尚高于原 DCQCN 的 26.828482 ms，不能宣称全部性能问题解决。竞争场景 B 对受下游竞争的流记录 16 次原生反馈、3 次降速，另一跨域流均为零。

多流检查另发现接收端 `ReceiveUdp` 将任意非零 ECN（包括 ECT(0)）当作拥塞：A 清除源侧 CE 后改为 ECT(0)，会再次触发 ACK-CNP。上述多流运行中 SOURCE_CE 和 B NATIVE_CNP 都为 8,967 次。因此上述单因素结果证明 B 自我降速循环在 M0 消除，但不证明 A 反馈边界已完整隔离。接收端兼容修正的决策与复测另行记录。

## 历史增量实现与复测：A 边界、发送许可、乐观启动（仍保留 B 标记）

本节保留早于 B 禁用新标记修订的实现与失败记录；本节的“本次”“当前”仅指该历史阶段。
其中 B 仍标记的行为与未完成结果不能当作当前策略说明，现行行为见上方源码核对。

### 已实现

- 默认 `PROPOSED_ECN_MODE source-boundary`：A 对注册 Source 流的上游 CE 置近端待反馈，改为 ECT(0)，保持 DSCP、包长度及解析缓存一致；按全局校验和设置重新计算 IPv4 校验和。A 受控出口不新标记，B/下游和非受控转发保持标记。
- 近端反馈合并 upstream_ce/local_queue/both 来源。队列为空不丢弃上游 CE，近端/原生反馈限频期间保留 pending；实际发出近端 CNP 后消费。日志新增 SOURCE_CE、NEAR_CNP_DEFER 及来源、CE/近端反馈计数和接管状态。
- 活动流瞬时空队列不撤销合法候选/整形速率。新包由已有设备发送路径按许可调度，仍受端口分配、有限令牌突发和 PFC 限制。
- 用户确认 `PROPOSED_STARTUP_MODE optimistic`：A 初始候选为 min(A/B 出口物理容量)，B 首次活动从出口物理容量初始化；均受各自端口联合分配约束，不设额外启动字节额度、不消耗探测桶。首份有效活动 STATE 接管（包括活动零预算）；启动前无需求 STATE 不跳过启动。start+既有 STATE_TIMEOUT 后仍未接管则停止，正常失效和 B 重启不重新授予乐观启动。
- `probe` 保留为保守启动对照。ECN `path` 模式允许旧 PROPOSED_SHAPER_ECN 的原始 A/B 同时开关语义；未显式设置 ECN 模式、boundary 模式携带旧键或关闭 near CNP 均明确拒绝。
- 未修改主机 DCQCN、普通交换机 ECN/MMU/PFC 或设备整形实现；B 标记规则按用户要求保留，尚未处理 B 自身整形反馈循环。

### 已执行检查

- `./ns3 build longhaul-convergence longhaul-r4-test -j2`：通过。
- 控制器/设备确定性回归：通过；新增 CE 消费/头部/校验和、低积压触发、合并/限频保留、实际 A/B 出队标记、正常和启动许可下 tick 间到包立即发送、暂停/尾部、活动零预算接管、无反馈超时及重启不重复乐观启动。
- 11 项独立进程拒绝测试：原 7 项及 boundary-no-near、boundary-no-ecn、ecn-mode、startup-mode，全部按预期拒绝。
- 接线/配置检查：缺 ECN_MODE、boundary 携带旧整形键、boundary 禁用近端反馈均按预期拒绝；显式 path+probe+旧整形键的双向小流正常完成。
- `python3 examples/LonghaulCC/test-longhaul-analysis.py`：8 项通过。
- 现有分析器成功处理本次 4 个场景；CSV 字段完整，各决策采样 in−tx=queue，所有端口目标不超当期容量，最终每条跨域流 A TX=B ADMIT。该守恒不表示所有流已经完成。
- `git diff --check` 和本轮新增行空白检查通过；历史源码的既有空白未顺手重排。

### 真实拓扑复测（seed=1、run=1、停止于 180 ms）

结果目录：`results/r4-boundary-startup-validation-20261008-_voftv_x/`。输入、有效配置、源码/构建身份及原始日志由 runner 独立保存，未覆盖旧结果。摘要见 [verification-summary.json](../../../../results/r4-boundary-startup-validation-20261008-_voftv_x/verification-summary.json)。

| 场景 | 完成 | admission drops | 判断 |
|---|---:|---:|---|
| M0 单流 200 MB | 0/1 | 0 | **性能/完成验收失败**，不能报告有效 FCT |
| 8 条同时启动、各 20 MB（缩小 M1） | 8/8 | 0 | 收发闭合；FCT 104.987–165.545 ms，未证明性能改善 |
| 双向各 1 MB，PG 3/4 | 2/2 | 0 | 收发闭合，两个方向 FCT 均为 10.314479 ms |
| 两条共享 B 出口 30 MB + 下游本地竞争 200 MB | 1/3 | 0 | 两条跨域流未完成，不能认定竞争场景通过 |

所有场景均保留真实 host→leaf→spine→DCI 路径。runner 对两个未完成场景返回失败；没有延长停止时间来掩盖结果，也没有关闭 B 标记或修改主机恢复参数。

### M0 失败时间线与下一步边界

流在 10 ms 开始；A 第一包 ADMIT/TX 均为 10.049749 ms，原先约 10 ms 的 A 启动等待已消除。B 第一包 ADMIT/TX 均为 15.064790 ms。

A 初始许可 100 Gb/s，B 初始预算 98 Gb/s。B 队列在 16.5 ms 已达到 416,056 B；16.607819 ms 观察到首个原生拥塞反馈，16.65 ms 首次降预算，17.6 ms 反应上限降到 1 Gb/s。A 在 20.1 ms 从乐观启动切到正常控制，23.55 ms 进入 local_pause。A 没有观察到上游 CE；结合保留的 B 标记路径和队列/反馈时间线，问题定位于 B 整形积压与其降预算的循环，尚未做关闭 B 标记的单因素消融。

180 ms 时，主机只发送 117,184,000 B、接收 41,583,000 B（目标 200,000,000 B），B 剩余 77,255,416 wire B。B 采样队列峰值 96,369,888 B；无准入丢弃不等于积压健康或完成验收通过。

用户已决定本轮交付上述实现及失败证据，下一轮讨论 B 的反馈来源/控制设计。功能回归通过不能表述为 R4 性能目标实现；当前默认策略不应作为已验收的性能基线。

## 原始实现范围（以下为修订前记录）

- DciGatewayNode 删除运行时 Group、组历史和 Assembly；采用逐流身份/代次、反应、服务窗口、STATE/DEMAND、历史及生命周期，每个物理出口一次联合分配。
- QbbNetDevice 增加成功入队通知、完整批次控制、逐流探测额度和共享端口探测桶。全部额度在真实出队前扣减；重复 epoch 不刷新余额。普通 PG/PFC、MMU、队列 0 优先路径仍沿用已有机制。
- B 服务窗口累加实际出队、有积压墙钟时间和 PG 累计暂停差。A 的过期/缺历史停发、一次启动、受接收确认限制的恢复、尾部排空已接入；history/snapshot 使用相同历史有效性条件。
- version 4 协议采用独立完整记录与 MTU 分包。结构错误整包拒绝，语义错误逐条拒绝；不等待其他流或丢失的包。
- 新增 R4 接线、配置和测试目标；runner、分析、绘图及 README 同步。源码版本、工作区差异、源码归档、二进制/共享库哈希、输入副本及有效配置保存在每次运行目录。
- switch-node、rdma-hw、BEgressQueue 未加入 R4 状态或修改算法。
- 保留旧 R3 接线、测试、配置、论文脚本及文档。旧 R3 测试不参与当前构建。迁移前源码快照位于项目根目录 results/r4-implementation-20261008/r3-before/，包含 HEAD、工作区差异和源码归档。

## 构建与确定性检查

在 simulator/ns-3.39 执行：

~~~bash
./ns3 configure
./ns3 build longhaul-convergence longhaul-r4-test -j2
./ns3 run longhaul-r4-test --no-build
python3 examples/LonghaulCC/test-longhaul-analysis.py
~~~

实际结果：构建通过；控制器/设备回归通过；Python 分析测试 8 项通过；git diff --check 通过。

控制器/设备回归包含：

- 带上限分配、空集/零、四流等份，以及 120 流满容量时的浮点累计上界。
- 普通 PFC 续发/恢复、自动到期、不同 PG、逻辑队列跳过与队列 0 优先。
- 成功入队计数与 MMU 拒绝不计入 ADMIT。
- 批量应用时首个实际出队已经看到所有队列的新速率。
- 非整帧倍数的逐流额度、共享探测桶包络、出队前扣减；重复 epoch、模式切换、无确认新报告不续发额度，接收 watermark 只消费一次。
- 积压/空闲服务时间、窗口边界与淘汰、busy-observed/budget 模式。
- 独立流报告、25 条 STATE 的三包往返、版本/长度/代次/序号/字段拒绝；一条非法记录不阻塞同包合法记录。
- 无数据 DEMAND 触发预算；高积压零服务不绕过排空；失效和缺历史停发、新报告恢复、停发时仍生成报告。
- CNP 窗口合并且不直接修改其他流；A 不执行 B 的乘法反应。
- Source 完成幂等、Receiver 不直接接受主机完成、DEMAND 完成不可倒退、已有尾部继续有预算。
- 历史脉冲、部分边界桶、过期覆盖；端口目标队列和源端总水位上界。

另外用独立进程验证以下输入均以明确错误终止：重复 ID、线身份复用、第 121 条出口流、同一端口混合角色、缺项控制批次、物理容量超限、timeout 不足。对应参数是测试可执行文件的 duplicate、identity、queues、mixed、batch、capacity、timeout。

runner 明确拒绝旧 R3 配置用于当前 Proposed、--r3-queue-mode 用于 R4，以及源码/构建身份不匹配的 --skip-build。拒绝证据见结果目录 runner-rejections.json。

## 真实拓扑功能运行

结果根目录：项目根目录 results/r4-implementation-20261008/。
保留早期 smoke/、validation/；后续模式检查在 final-* 和 competition-*。
verification-summary.json 汇总这些后续运行，逐 run 保留原始数据和身份信息。

| 输入 | 作用 |
|---|---|
| r4-single：1 MB 单流 | 无竞争完成闭环 |
| r4-bidirectional：两个方向各 1 MB、PG 3/4 | A/B 角色和 peer 分别对应各自方向 |
| r4-shared-port：0→41、0→59 各 30 MB | 确认两流共用 B 出口 81→79，之后分别经 Leaf 73/75 |
| r4-heterogeneous：上述两流加 40 MB 本地流 | 有限流接线/完成检查；不据此认定竞争持续时间充分 |
| r4-competition：上述两流加 200 MB 42→41，20 ms 开始 | 延长本地供给，使其与跨域到达重叠，仅在指定下游接收路径相交 |

竞争输入在 history/snapshot × busy-observed/budget 四种组合下均运行；普通 DCQCN 做了同输入回归。停止时间统一 0.18 s，seed/run 均为 1。最后对满容量舍入修正重跑了 final-capacity-regression/。

检查结果：

- 每次运行期望 FCT 条数与实际完成数一致，主机唯一发送 payload 和接收 payload 等于输入大小。
- 每条注册流 A/B 入队与出队闭合，A wire TX 等于 B wire ADMIT；无 admission drops，停止时全网交换机队列为零。
- 端口各流目标总和不超过当期可用容量估计。
- 使用原始事件重建，报告 sample ordinal 与 B 累计计数/队列相符；A TX 到 B ADMIT 的固定帧前向时延映射没有不匹配。
- 预测终点取该纳秒全部 B 事件之后；未来实际收发计数得到的离线 oracle 队列与事件队列相等。oracle 仅检验守恒，不是在线预测效果。
- 公共图和 R4 逐流控制图可以生成。逐流 target/service 曲线标明为控制目标/服务估计，不替代实际吞吐。

例如在项目根目录复跑一个独立结果目录：

~~~bash
python3 simulator/ns-3.39/examples/LonghaulCC/run-longhaul.py \
  --algorithm proposed \
  --config simulator/ns-3.39/examples/LonghaulCC/config-longhaul-r4.txt \
  --flow-files results/r4-implementation-20261008/inputs/flow-longhaul-r4-competition.txt \
  --queue-mode history --service-mode busy-observed \
  --purpose completion --stop-times 0.18 --output-root /tmp/r4-review
python3 simulator/ns-3.39/examples/LonghaulCC/analyze-longhaul.py --root /tmp/r4-review
~~~

## 结论边界

这些检查验证功能与计量闭环，不宣称已经证明吞吐隔离、预测增益、长期满端口饱和稳定性或硬件可部署性。
控制丢失/乱序/异常字段通过确定性记录输入与状态测试覆盖，未做随机控制丢包压力矩阵。
未对 HPCC/TIMELY/Bifrost 的拥塞性能重新验收，也未修改其算法或参数。
首版仍是固定注册/路由/帧时延假设，每端口最多 120 条受控流；源/接收端角色不能在同一受控物理出口混合。
不同龄期远端预算可能暂时超出 B 当前份额；A 本端分配上界不是 B 容量预留。
