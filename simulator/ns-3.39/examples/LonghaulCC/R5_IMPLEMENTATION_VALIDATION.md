# R5 方案一：实现与功能验证（2026-10-14）

实现依据为项目根目录 `docs/R5设计方案一.md`（含服务估计、预测峰值和 CNP 等待/滞回细化）。
没有采用方案二/三的短长流分类、B 端附加调速等机制。本记录区分实现、验证和待验证效果。

## 1. 模块职责及实施顺序

1. 检查 AGENTS、工作区、方案与实际调用路径。开始时只有四份未跟踪设计文档及用户删除的两份 R3 输入；这些改动均保留。
2. 先实现数据面的组级共享令牌桶，替换只适合旧控制器的逐流整形/探测接口；保留 PG/MMU/PFC 和现有逻辑队列轮询。
3. 重写网关核心，分离 B 测量/反馈与 A 预测/决策/本地反压；再通过真实路由接线。
4. 删除旧入口、完成通知、DEMAND 与旧测试；同步构建、runner、分析、绘图、场景引用及使用文档。
5. 确定性机制回归 → 真实拓扑有限流量 → 逐包控制/发送审计 → 六种基线验证。

| 模块 | R5 职责和接口 |
|---|---|
| `longhaul-convergence.cc` | 创建拓扑、配置主机、调度流、调用 R5 Setup/Finish；完成记录仅用于公共测量 |
| `longhaul-routing.h` | 公共 `TracePath/RoutePacket/Peer/PathDelay`，供所有算法的测量及 R5 接线使用 |
| `longhaul-r5.h` | 从真实 ECMP 路径识别 A、B、B 出口与 PG；提供最小连接身份和源 QP 关联 |
| `DciGatewayNode` | 组状态、B 服务统计与报文、A 实际发送历史和控制、近端 CNP、证据日志 |
| `QbbNetDevice` | `ConfigureInjectionGroup/BindInjectionQueue/ApplyInjectionControls`；原子更新整端口组速率，共享额度在实际出队时扣除 |
| `SwitchNode` | 普通转发和原生 ECN；新增默认无动作的准入丢弃通知，以供 R5 核算 |

核心数据路径为：成功准入 → A 流逻辑队列 → 组共享令牌桶 → 实际 WAN 出队/历史记录 →
B 原生 PG 队列 → 正常调度出队。控制路径为：B 周期采样 → 独立优先级 0 的网络报文 →
A 验证序号/时间/累计计数 → 重放 → 计算组速率 → 端口分配 → 原子应用发送约束。

## 2. 与方案的对应关系

| 方案 | 实现 |
|---|---|
| §2 A 控制、B 测量 | 只有 A 创建整形组；B 没有速率上限控制或基于 CNP 的额外反应环 |
| §3 控制域及字节口径 | `(B 节点、实际出口、PG)` 标识；Q 为完整 PG 队列，包含非受控到达；按完整序列化报文大小计数，payload goodput 单独测量 |
| §3.1 服务估计 | 每次出队检查是否排空；独占且未暂停使用容量；共享且全区间积压、无暂停/容量变化时用实测出队率；空闲保留有效估计；当前完全暂停报告 0，已知速率变化使旧估计失效 |
| §4 状态反馈 | 一个组一条固定长度版本 5 STATE；含时间、序号、队列、到达/出队/丢弃、非受控到达、服务、容量、缓存预算、暂停/排空状态与采样事件边界；数据停发不停止反馈 |
| §5 重放 | 只在 A 实际 WAN 出队记录历史；每个包的预测到达时间包括真实串行化、传播及接收处理；逐事件减服务、截零、再加到达，避免空闲服务抵消后续突发 |
| §5 同纳秒边界 | B 另反馈累计受控到达（含准入丢弃）；结合历史累计发送区分同一纳秒内采样前后的报文，防止漏算/重复计算 |
| §5.1 峰值 | 同次重放计算 `[t,t+d_f]` 的最大队列；接近预算禁止增加并保守控制，达到预算停发；新有效反馈且重新留有余量后解除 |
| §6 控制律 | `clip(max(c-b,0) - (Qpred-Qref)/T, 0, C_A)`；服务明确停止时强制 0；同一物理出口按候选上限做 max-min 分配 |
| §7 不确定性 | 反馈超时/历史缺失停发；在线冻结下一次尚未发生的 B 周期样本预测，后续反馈用于残差检查，连续 3 次超余量时降低信任；恢复依赖新状态 |
| §8 有限缓存 | 配置共享池 80% 等分给控制域，并按全节点当前出口占用进一步限制；不重复分配全池，不将 headroom 当作控制器可重复使用的空间 |
| §9 本地 CNP | 同一观察窗的输入/输出差额；压力 `Q + max(growth,0)*wait + margin`；高低水位、普通等待、停止追加、重新增长、紧急最小间隔；只通知当前窗口确实贡献输入的 QP |
| §9.2 QP | 使用源 IP/源 UDP 端口/PG，而非目的 QP；实际网络 CNP 指向源主机 |
| §10 原生反馈 | 保留已有 CE、CNP/ACK-CNP；普通 ECN 继续生效；不以原生 CNP 判定拥塞位置或压低 B 出队 |
| §11 冷启动/调度 | 冷启动共享保守组速率；一组一个最大报文额度，新增流不另获突发额度；组内只轮询有包且 PG 可发的流，空流不保留固定份额 |

执行延迟取 0：控制在同一仿真事件中原子更新发送资格，已经出队的报文在历史中。
决策终点使用最大数据报文的完整前向时延；短尾包使用自身长度计算实际到达。
同一最大报文串行化范围内的离散差异、ns-3 时间量化和未来服务变化由误差余量覆盖，不能视为严格误差界。

不存在流大小/起始时刻/需求量注册或完成通知。连接和组关联固定到仿真结束，空队列不占发送份额；
历史只保留反馈有效期所需的到达记录。当前设备最多允许每个 A 物理出口 120 个流逻辑队列。

## 3. 明确选定的基础参数和边界

方案未指定数值的部分采用可检查的初始配置，未做参数优化：

- 控制周期 50 µs，反馈/本地观察窗 200 µs，`T=2 ms`，反馈超时 20 ms。
- `Qref=50,000 B`，风险余量 65,536 B；不是 WAN BDP。
- 冷启动为本地物理出口的 1%，不确定状态最多 5%；陈旧状态选择停发这一保守策略。
- 预测风险达到组预算的 90% 时禁止增加并保守控制，达到 100% 停发。
- 本地高/低水位 250,000/62,500 B，并受实际预算限制；普通等待至少 300 µs，
  且不小于真实本地来回路径与主机减速定时器估计；同 QP 最小间隔 50 µs。
- 一个观察窗下降不足一个最大报文视为“基本不下降”，避免单包离散波动误判稳定排空。

固定管理路径、单对直连网关、一个 A 负责一个 B 控制域。发现连接出口与关联不一致就失败退出，
需要重新关联；没有实现动态改路恢复、多入口预算协调或独立下游限速。
缓存预算是共享资源上的控制政策，不是硬件独占预留。原生 ECN/CNP 可能与网关控制耦合。
组内是报文级轮询，不承诺不同报文长度下的严格字节公平。

## 4. 修改与删除清单

新增：

- `longhaul-r5.h`、`longhaul-routing.h`
- `longhaul-r5-test.cc`、`verify-longhaul-r5.py`
- 本文件 `R5_IMPLEMENTATION_VALIDATION.md`

修改：

- `src/point-to-point/model/dci-gateway-node.{h,cc}`：重写控制核心。
- `src/point-to-point/model/qbb-net-device.{h,cc}`：共享组预算接口和实际扣除。
- `src/point-to-point/model/switch-node.{h,cc}`：丢弃观察，移除旧 ECN 特例钩子。
- 本目录 `longhaul-convergence.cc`、`longhaul-measurements.h`、`CMakeLists.txt`。
- `run-longhaul.py`、`analyze-longhaul.py`、`plot-longhaul.py`、`test-longhaul-analysis.py`。
- `experiment-scenarios.json`：清理对已删除 R3 mechanism 输入的运行契约。
- `README.md`、`LONGHAUL_USER_GUIDE.md`；R4 验证文档只加历史标记。

删除 `longhaul-r4.h`、`longhaul-r4-test.cc`；旧 R4Header、DEMAND、CompleteFlow、
两端逐流反应/整形、CE 转换、逐流探测额度、旧分析分支一并移除。
`flow-longhaul-r3-mechanism*.txt` 的删除是用户已有改动，本次未恢复或改写。
`config-longhaul.txt`、正式拓扑/流量输入、四份设计文档和基线算法实现未改动。

## 5. 验证命令与结果

工作目录为 `simulator/ns-3.39`：

```bash
./ns3 build longhaul-convergence longhaul-r5-test -j2
./ns3 run longhaul-r5-test --no-build
python3 examples/LonghaulCC/test-longhaul-analysis.py
```

构建通过；C++ 的 10 组检查通过，覆盖重放/同纳秒采样/峰值、反馈协议、服务状态、局部反压、
共享实际出队、背景到达/丢弃、完整 CNP 状态过程、CE 透传、端到端暂停/恢复、接收端 ECT/CE。
端到端检查通过真实网络状态报文让 A 停止实际发送，验证数据停发仍有反馈，之后恢复并闭合 1,048,000 B。
Python 8 项分析测试通过；相关脚本 `py_compile` 和 `git diff --check` 通过。

真实拓扑验证从项目根目录执行；`--output-root` 在复现时须改为新目录：

```bash
python3 simulator/ns-3.39/examples/LonghaulCC/run-longhaul.py --algorithm proposed \
  --flow-files results/r5-validation-20261014/inputs/flow-r5-shared.txt \
               results/r5-validation-20261014/inputs/flow-r5-bidirectional.txt \
               results/r5-validation-20261014/inputs/flow-r5-incast.txt \
  --stop-times 0.15 --purpose completion \
  --output-root results/r5-validation-20261014/verified

python3 simulator/ns-3.39/examples/LonghaulCC/run-longhaul-all.py \
  --algorithms dcqcn hpcc timely bifrost \
  --flow-files results/r5-validation-20261014/inputs/flow-r5-shared.txt \
               results/r5-validation-20261014/inputs/flow-r5-bidir-pg3.txt \
  --stop-times 0.15 --purpose completion --skip-build \
  --output-root results/r5-validation-20261014/verified

python3 simulator/ns-3.39/examples/LonghaulCC/verify-longhaul-r5.py \
  --root results/r5-validation-20261014/verified
python3 simulator/ns-3.39/examples/LonghaulCC/analyze-longhaul.py \
  --root results/r5-validation-20261014/verified
python3 simulator/ns-3.39/examples/LonghaulCC/plot-longhaul.py \
  --root results/r5-validation-20261014/verified --all-scenarios
```

当前真实拓扑有 82 节点，WAN 300 Gb/s、5 ms，B 内向出口 100 Gb/s。直接读取并记录了实际配置，未用历史配置推断。

| 验证 | 结果 |
|---|---|
| R5 3 次运行 | 16/16 流完成；包含短尾包、不同开始时刻、双向、多 PG 和 8 流 incast |
| 实际同组关系 | 8 流 incast 中有 5 流落到 B 同一出口/PG；按 `r5.paths.csv` 核对，未按共同目的地址臆测同组 |
| 四基线各 2 次 | DCQCN/HPCC/TIMELY/Bifrost 共 32/32 流完成 |
| 逐流字节 | 上述 48 条流的唯一 TX、实际 TX payload、RX 均等于声明大小，无重传 payload 增量 |
| 全网资源 | 11 次运行均为 0 准入丢弃、0 终态队列、0 MMU 入口/出口残留 |
| R5 控制与发送 | 45,004 个实际 WAN 数据包逐个通过组令牌扣除审计；端口速率和不超过物理容量 |
| R5 路径/反馈 | 实际发送至 B 到达的时刻和长度匹配；全部有状态记录的采样快照与真实事件边界一致；无意外反馈拒绝 |
| 分析/绘图 | 成功生成公共汇总、R5 预测证据和控制图；B 不绘制不存在的整形目标 |

另直接调用相同二进制的 `--cc=frp`、`--cc=rocc`，在共享目的端有限流输入上各完成 4/4 流，
同样通过 TX/RX、丢弃及终态队列/MMU 检查。配置、命令、构建哈希及结果保存在
`results/r5-validation-20261014/additional-baselines-verified/{frp,rocc}/validation.json`。
合计 13 次有效运行、56 条完成流。没有为了覆盖这两种模式而增加 runner 功能。

初次给 Bifrost 运行混合 PG 输入时，被既有的 `flow.pg != bifrost_priority` 检查拒绝（固定 PG=3）。
保留该失败记录，在其支持的 PG=3 双向输入补测通过；未改动基线约束。
早期调试结果不是最终验收依据，最终使用 `verified/` 及 `additional-baselines-verified/`。

最终证据目录：
`/home/shemuping/newCode/ns3-FRP/results/r5-validation-20261014/verified`。
其中 `r5-validation.json` 保存逐流字节和机制核对，`r5-evidence.csv` 保存当时预测与之后真实状态的比较，
各 run 保存实际配置、输入、源码/二进制/共享库身份。补充基线在上述同级目录。

## 6. 未验证的效果

没有进行大范围调参、长时间压力扫描或性能排名。有限样本中 B 队列通常很小，不能据此推出
预测精度、吞吐、FCT、公平性或总排队优于基线。正常服务下的完成功能不证明任意突发/反馈中断下无丢包。
仍需独立研究服务骤变强度、持续供给不足与恢复、时延误差、原生 CNP 耦合、A/B/下游总排队和鲁棒边界。
本轮没有尚待用户确认的阻塞项；上述为性能研究和适用范围边界。

## 7. 空组恢复修复与方案一致性复审（2026-10-15）

本节以 `docs/R5设计方案一.md` 为审查依据。只修复空组恢复；不修改设计文档、
控制参数、原生 ECN/CNP 或其他待讨论机制。此前工作区改动保留。

### 已修复及验证

原 `CommitControls()` 将空组 target 设为 0，新包到达不会重新分配，要等下一次
50 us tick。现在源组从空变为非空且 target 为 0 时，安排同一仿真时刻的激活事件；
事件在准入回调结束后重新计算源组候选速率，再原子应用端口总预算。
暂停、陈旧状态、历史覆盖及预测风险检查仍由 `UpdateSource()` 执行；不额外增加突发令牌。

- 新增 `IdleActivation()`：51 us 时两个空组同时收到首包，52 us 前均实际发出；
  检查总预算不超端口容量、暂停不可绕过、恢复可排空、过期反馈不可绕过。
- C++ 全部 11 组机制回归、Python 8 项分析测试和 `git diff --check` 通过。
- `./ns3 build` 被本地 lock 中的 debug 与 CMake cache 中 optimized 不一致阻塞；
  没有改动用户构建设置，使用已有 cache：
  `cmake --build cmake-cache --target longhaul-r5-test longhaul-convergence -j2`。
  随后直接运行 `build/examples/LonghaulCC/ns3.39-longhaul-r5-test-optimized`。
- 有限流量 4/4 完成，逐流 TX/RX、WAN 字节闭合、0 丢包、0 队列/MMU 残留；
  `verify-longhaul-r5.py --root results/r5-idle-reactivation-20261015/finite`
  通过 6,001 个实际 WAN 数据包的共享令牌审计。

结果根目录：`/home/shemuping/newCode/ns3-FRP/results/r5-idle-reactivation-20261015`。
本次绕过有 profile 不一致的 ns3 启动包装器，直接调用已构建二进制；各运行保存独立配置、
输入、副本哈希、源码/二进制/共享库哈希、实际命令和 stdout。

S1 使用 `longhaul-261015-0927` 的输入与配置（400 Gb/s WAN、200 ms、相同 seed/run），
重新运行 R5 与 DCQCN。10–200 ms 合计接收 goodput：修复后 R5 60.0345 Gb/s，
DCQCN 66.2524 Gb/s；旧 R5 为 62.1628 Gb/s。两次新运行均 0 准入丢包、0/3 完成。
功能修复没有带来这次吞吐提升，不能将其表述为性能优化已经成功。

按事件顺序，将空组首个 ADMIT、此前零速率 CONTROL、随后 normal 正速率 CONTROL 对齐：
旧运行 3,237 次的恢复等待中位数为 49,871 ns；修复后 2,603 次全部为 0 ns。
这测量的是恢复额度等待，不是物理发送/传播时延；保护性停发不计入该统计。

### 其余发现：尚未修改

1. **高优先级：残差保守上限混用了调度结果。** `UpdateSource()` 的
   `min(candidate, target, capacity * conservativeFraction)` 在 target 为 0 时仍给出 0。
   target 可能来自空闲或此前服务停止，而非当前服务能力。新 S1 的 1,672 个
   `residual_conservative` 周期样本，原控制律候选值均为正，但最终候选/target 均为 0。
   方案 §7 要求保守上限、新反馈恢复，没有要求用上次实际调度额度作为不可增加上限。
   建议将预测安全上限与端口调度份额分开考虑，并为残差状态定义明确恢复规则；
   不直接删除峰值保护或为所有零速率设置正下限。
2. **需要澄清的状态语义：** `protecting` 只被设置和写日志，`NeedCnp()` 不读取它，
   Q_L 只影响该标记；实际通知取决于高水位、增长、等待和紧急条件。
   因此不能把日志中的 protecting 当作独立执行的滞回控制。它不自动证明多发了 CNP：
   方案 §9.1 的触发与停止追加条件仍需逐条对照，明确该标记是观测状态还是控制状态。
3. **实现选择而非方案固定值：** 250/62.5 KB 水位、65,536 B 余量、300 us 等待、
   1%/5% 冷启动/保守比例、90% 风险门槛、80% 等分池预算均是本实现取值。
   短窗口净增长可能受采样相位影响；持续积压和主机响应需要事件证据，不能仅凭
   protecting 或单次压力越界推断。先修正语义问题，再做单因素参数验证。
4. **公共配置校验疏漏：** ECN 速率映射缺项仅用 `NS_ASSERT_MSG` 校验，optimized
   关闭断言后 `map[rate]` 可插入零阈值。本次配置覆盖 400 Gb/s，但其他速率仍有潜在风险；
   应改为始终有效的输入检查，此轮未扩展修改公共初始化。

保留原生 CE/CNP、B 不额外整形、明确暂停时停止注入、按出口/PG 分组、实际历史重放，
均有方案依据；不能为了消除这次吞吐差距直接删除。方案 §10 已承认原生反馈与网关
控制的耦合，§2/11 也没有承诺下游共同瓶颈上的逐流公平。

## 8. 保护恢复、暂停服务、定向反压与组内调度修复（2026-10-15）

本节记录第 7 节之后的实现，当前参数和行为以本节为准。用户授权修复 P0/P1 三项问题，
并在 S1 对照暴露组内调度偏置后，明确将调度修复纳入本轮。
B 独立下游主动限速仍作为后续工作：B 正常转发、测量反馈、受下游 PFC 约束，原生 CNP 透传。
设计文档、输入拓扑、原生 DCQCN 及公共主机恢复参数未改动。

### 实现

1. **分离预测上限与调度份额。** `candidate` 是预测和保护给出的速率上限，`target` 仅是
   当前端口分配。残差保护取 `min(本轮控制律候选, 物理容量 * 5%)`，不再读取上一轮 target；
   峰值预警额外限制不超过上一轮 candidate，预算耗尽仍停发。明确暂停、零服务、反馈过期、
   历史缺失仍可令速率为 0，没有给所有停发状态添加正下限。
2. **分开估计未暂停服务与可用时间。** B 统计实际暂停时长，令
   `available_fraction = (采样间隔 - 暂停时长) / 采样间隔`。独占出口使用已知物理容量；
   共享出口持续积压时，用 `区间出队字节 / 未暂停时间` 更新服务能力；曾排空时保留有效估计。
   未暂停状态反馈 `未暂停服务能力 * available_fraction`，当前明确暂停仍反馈零服务。
   因此部分暂停不再被“采样时恰好已恢复”掩盖，也不把供给不足直接当成低容量。
3. **加快有效状态反馈。** 设备新增按 PG 的实际暂停状态变化 trace，包含 PFC 到期恢复；
   B 在变化后至多 50 us 补采反馈，期间合并多次变化，PFC 刷新不重复触发。周期反馈仍为 200 us。
   A 收到合法新状态后，在同一仿真时刻安排预测、风险检查和预算重分配，不再额外等 50 us tick。
   这些时限不包含网络传播和发送排队。
4. **减少本地追加反压。** 普通通知每组每轮只选择净积压贡献最大的活跃 QP；同组追加普通
   CNP 至少等待 `max(300 us, 本地响应时间)`。选择贡献者之后，再检查该 QP 已收到的近端或
   原生 CNP 的等待时间，避免因为主要贡献者尚在等待而转向较小贡献者。原生 CNP/ACK-CNP
   不丢弃、不改写。下降停止追加、紧急通知所有活跃贡献者及同 QP 50 us 最小间隔仍保留。
   本地高/低水位由 250,000/62,500 B 改为 1,000,000/250,000 B，仍分别受当前预算的
   50%/25% 限制；压力计算继续包含等待期间的增长与误差余量。
5. **每个整形组独立轮询。** 原先三个逻辑队列按 `组一/组二/组一` 排列时，组二出队会推进
   全端口轮询位置；组一下一次令牌就绪时，可能反复优先服务其同一个成员。现在每组独立保留
   下一成员位置，每次只提名一个非空成员给端口调度，且仅在该组实际出队后推进。
   空成员不占额度，零速率和 PFC 不推进位置；每组仍共享一个最大报文突发额度和同一 PG。
   不修改无整形出口的调度，不将组预算复制成逐流预算。

组 CSV 增加采样间隔、未暂停服务能力和可用时间比例；metadata schema 升至 2，并记录
反馈、反压及组内调度方式。R5 网络协议版本与 17 个反馈字段保持不变。

### 回归和复现

以下三项用例均先在未修复的对应实现上失败，再在修复后通过：保护期间从零份额恢复实际发送；
等待主要贡献者时不将 CNP 转嫁给较小贡献者；两组交错排列时同组持续积压成员交替服务。

- C++ 15 组机制回归通过，覆盖部分暂停、共享空闲估计保留、真实 PFC 到期恢复、刷新合并、
  新反馈直接激活实际发送、CNP/ACK-CNP 透传与等待、紧急保护和组共享令牌。
- Python 8 项分析测试通过。沿用已有 optimized CMake cache 构建，不改变本地 profile 设置。
- 最终实验保存在 `results/r5-control-recovery-20261015/verified`；上级目录中的其他运行是
  调试中间版本，不作为最终验收结果。各运行保存实际配置、输入、副本哈希、源码/二进制/
  共享库身份、执行命令和 stdout；`implementation/before` 与 `implementation/after` 保存本轮相关文件。

```bash
cd simulator/ns-3.39
cmake --build cmake-cache --target longhaul-r5-test longhaul-convergence -j2
build/examples/LonghaulCC/ns3.39-longhaul-r5-test-optimized
python3 examples/LonghaulCC/test-longhaul-analysis.py
```

在项目根目录验证运行结果：

```bash
python3 simulator/ns-3.39/examples/LonghaulCC/verify-longhaul-r5.py \
  --root results/r5-control-recovery-20261015/verified/finite
python3 simulator/ns-3.39/examples/LonghaulCC/analyze-longhaul.py \
  --root results/r5-control-recovery-20261015/verified
python3 results/r5-control-recovery-20261015/verified/compare-s1.py
```

最终的共享目的端、双向流和 incast 共 **16/16 流完成**，逐流唯一 TX/RX 和 WAN 字节闭合，
0 准入丢包、0 终态队列/MMU 残留。45,004 个实际 WAN 数据包通过组共享令牌审计；
实际到达映射、采样事件边界和反馈合法性检查均通过。

S1 使用第 7 节保存的同一配置和输入：400 Gb/s WAN、5 ms、三条 3 GB 流、相同 seed/run、
停止时间 200 ms。下表统计 10–200 ms，字节数以终态计数闭合 CSV 尾部；所有 MB 为十进制。

| 指标 | 本轮修改前 | 本轮最终版本 |
|---|---:|---:|
| 合计接收 goodput | 60.0345 Gb/s | 62.7243 Gb/s（+4.48%） |
| 合计实际发送 payload | 64.3358 Gb/s | 63.2377 Gb/s |
| 近端 CNP 发送数 | 283 | 40 |
| 同组流 1 / 3 接收 goodput | 15.1161 / 15.0309 Gb/s | 15.7638 / 15.8613 Gb/s |
| 残差保护中有积压、原始候选为正却最终候选为零的周期样本 | 1,672 | 0 |
| A 单组实际队列峰值的最大值 | 1.2304 MB | 3.8881 MB |
| B 单组实际队列峰值的最大值 | 45.9276 MB | 56.1780 MB |
| A WAN 端口收到 PFC 后的实际暂停覆盖时长 | 81.0481 ms | 88.9592 ms |
| 准入丢包 / 完成流 | 0 / 0/3 | 0 / 0/3 |

最终版本有 1,115 个残差保护样本同时满足“有积压、原始候选为正”，候选均能保持正值；
仍有由明确服务停止、峰值风险等引起的停发，不能把所有零速率都视作缺陷。
暂停时长按接收端 pause/resume 事件与配置的 5 us 过期时间重建并合并刷新区间；
S1 只有 PG 3 数据流，不使用 PFC 刷新行数替代时长。

这次收益不是发送端整体加速，也没有证明优于原生 DCQCN：同一对照中的 DCQCN 接收 goodput
为 66.2524 Gb/s。B 队列峰值和 PFC 覆盖上升，表明仍依赖 PFC/headroom；峰值风险检查并非
对未来服务未知变化的硬缓存保证。S1 三条长流均未完成，仅支持瞬态比较，不支持 FCT 或完成性结论。
组内修复仅保证同组持续积压成员的报文轮询，不承诺不同控制域在下游共同瓶颈上的逐流公平。

### 主机恢复参数评估

保留 `RPTimer=900 us`、`FastRecoveryTimes=5`、`RateAI=50 Mb/s`、`RateHAI=100 Mb/s`。
源码确认每次实际减速会重置恢复阶段并重新安排 900 us 定时器；但不能将网关观测到的 CNP 数
直接换算成主机减速次数。最终 S1 的 A 仍观察到 1,411,629 次原生 CNP/ACK-CNP，各流最后一次
约在 198.06–198.12 ms，且 WAN 暂停覆盖仍有 88.96 ms。

因此，本轮先保留主机参数，不用加快恢复掩盖仍存在的服务变化和下游反馈耦合。当前没有采集
主机 target/alpha/恢复阶段的事件轨迹，不能声称已确认恢复计时器是剩余差距的根因。
`host-recovery-assessment.json` 记录参数、源码行为、A 侧反馈间隔及该观测限制；
`s1-comparison.json` 保存匹配窗口、逐流 TX/RX、队列峰值、暂停时长和停发原因统计。

最终证据绝对路径：
`/home/shemuping/newCode/ns3-FRP/results/r5-control-recovery-20261015/verified`。
