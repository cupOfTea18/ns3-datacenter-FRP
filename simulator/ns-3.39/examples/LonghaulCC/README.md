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
- Proposed R4：`proposed`

RoCC 在 40/100 Gbps 链路上使用论文参数。论文未给出 200 Gbps 参数；当前实现按
带宽比例放大 `Qref/Qmid/Qmax`，并保留 100 Gbps 的 PI 基础增益，该策略会写入运行元数据。

程序读取拓扑、公共配置和流量场景文件，创建 Qbb/RDMA 网络，配置路由和流，
最后将吞吐率、FCT、DCI 链路等结果写入结果目录。

所有命令默认在 ns-3.39 根目录执行：

```bash
cd /home/shemuping/newCode/ns3-FRP/simulator/ns-3.39
```

### 本机路径适配

`run-longhaul.py` 根据脚本所在位置自动定位 ns-3 和项目根目录，项目移动后无需修改
脚本中的绝对路径。`run-longhaul-all.py` 逐个调用它，并为所有算法使用同一个结果根目录。

| 路径 | 解析规则及本机位置 |
|---|---|
| ns-3 根目录 | 自动定位；当前仓库中为 `/home/shemuping/newCode/ns3-FRP/simulator/ns-3.39` |
| 默认配置 | 自动定位脚本同目录的 `config-longhaul-r4.txt`（旧 R3 配置保留） |
| 配置中的 `TOPOLOGY_FILE` / `FLOW_FILE` | 相对 ns-3 根目录；当前 `examples/LonghaulCC/...` 无需修改 |
| 命令行 `--config` / `--flow-files` | 绝对路径直接使用；相对路径先查启动目录，再查 ns-3 根目录 |
| runner 默认结果目录 | 项目根目录下的 `results/longhaul-YYMMDD-HHMM/` |
| 命令行 `--output-root` | 自定义结果目录；相对路径以启动目录为基准 |
| 配置中的各项 `*_OUTPUT_FILE` / `SUMMARY_META_FILE` / `PROPOSED_OUTPUT` | runner 自动改写为本次运行目录下的绝对路径 |

也可从项目根目录启动，例如：

```bash
python3 simulator/ns-3.39/examples/LonghaulCC/run-longhaul.py --algorithm dcqcn \
  --flow-files examples/LonghaulCC/flow-longhaul-s0.txt --stop-times 0.03
```

直接使用 `./ns3 run` 时，配置中的相对输入和输出路径以 ns-3 根目录为基准，
输出目录需预先创建：`mkdir -p results/longhaul`。这里的结果位于
`simulator/ns-3.39/results/longhaul/`，与 runner 的项目级结果目录不同。

仓库 `run_scripts/` 中还有旧程序的绝对路径，但它们不参与 LonghaulCC 运行链。

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

首次下载后，先配置为 runner 使用的 optimized 构建，再编译主程序：

```bash
./ns3 configure --build-profile=optimized --enable-examples --disable-tests \
  --disable-python-bindings --disable-werror
./ns3 build longhaul-convergence -j2
```

需要 CMake 3.10 或以上、C/C++ 编译器及 Make 或 Ninja。LonghaulCC 是 C++ 程序，
无需开启 ns-3 Python bindings；Python runner 已兼容本机 Python 3.10 的文件哈希计算。
若提示 `CMake not found`，Ubuntu 可通过 `sudo apt install cmake` 安装后重试。

另有 `longhaul-r4-test` 确定性控制器和设备回归目标。

## 4. 运行用途与场景

runner 默认读取 `config-longhaul-r4.txt`，其配置为 **S0、Proposed R4、0.38 s**，
运行用途由 runner 默认记为 `transient`；`--algorithm` 覆盖配置中的 `CC`。
直接运行 C++ 且省略 `--conf` 时仍读取旧 `config-longhaul.txt`，该文件为 S0、DCQCN、0.38 s；
直接选择 R4 必须指定 `--conf=examples/LonghaulCC/config-longhaul-r4.txt`。
`run-longhaul-all.py` 默认依次运行
DCQCN、HPCC、TIMELY、Bifrost、Proposed，每种一次；这不是完整 FCT 验收或已标定的比较矩阵。
S2 后加入的七条 3 GB 流只剩 0.17 s，物理上不足以全部完成。

批量执行链：`run-longhaul-all.py` → `run-longhaul.py` → `longhaul-convergence.cc` →
路径注册/网关控制/公共测量 → `analyze-longhaul.py` → `plot-longhaul.py`。
单算法直接从 `run-longhaul.py` 开始。批量运行首次调用默认构建，后续调用自动使用
`--skip-build` 跳过编译并复用现有程序；显式指定 `--skip-build` 时，首次调用也跳过编译。
批量入口的其他实验参数转交单算法入口，完整说明见 `run-longhaul.py --help`。
不迁移到仓库 `run_scripts/run_single_*` 旧入口。

```bash
# 启动检查，结果不用于完整 FCT 比较；每次使用新目录。
python3 examples/LonghaulCC/run-longhaul.py --algorithm dcqcn \
  --flow-files examples/LonghaulCC/flow-longhaul-s0.txt \
  --purpose transient --stop-times 0.03 --output-root /tmp/longhaul-s0-smoke

# 完成实验：1.50 s 只是候选，runner 会检查完成数、FCT 唯一性及 RX payload。
python3 examples/LonghaulCC/run-longhaul-all.py --algorithms dcqcn proposed \
  --flow-files examples/LonghaulCC/flow-longhaul-s2.txt \
  --purpose completion --stop-times 1.50 --output-root /tmp/longhaul-s2-completion
```

`--purpose` 默认为 `transient`，记录固定停止规则；`completion` 必须显式指定
`--stop-times`，未全部完成时 runner 返回非零。进程 `status=ok` 与
`completion_valid` 是两个不同字段，瞬态实验允许未完成，但分析必须显示未完成数量。
算法间使用相同输入和停止时刻，不能按效果分别选择时长。

S0–S5 正式输入未改动。新增输入及预先定义的窗口见 `experiment-scenarios.json`：

| 输入 | 设置与目的 |
|---|---|
| `flow-longhaul-s2-scaled.txt` | 首流 750 MB，10 ms 启动；七条 150 MB 于 20 ms 加入；观察 20–60 ms |
| `flow-longhaul-s3-scaled.txt` | 七条 1 MB、一条 1 GB，同在 10 ms 启动；退出时间保守取七条全部完成与实际 TX 停止，之后检查至少一个保持窗口 |
| `flow-longhaul-s5-scaled.txt` | 八条 500 MB 背景，四条 5 MB 探测于 20/22/24/26 ms 加入；观察 20–75 ms |
| `flow-longhaul-r3-mechanism.txt` | `0→41`、`0→59` 各 750 MB；30 ms 加入 `42→41` 的 500 MB 本地竞争 |
| `flow-longhaul-r3-mechanism-control.txt` | 相同两条跨域流，不加本地竞争 |

缩小输入用于事件关系验收，不是按统一比例缩放的正式负载。
特别是 S3 的原 4.8:1 比例未用于此事件测试；保留七条先退出、一条仍供给的关系，
正式 `flow-longhaul-s3.txt` 仍为 625 MB/3 GB。首版候选及失败记录保存在验证目录，不能与修订输入混用。
默认保持判据仍为 ±10%、`max(3×base RTT,20 ms)`，跨域约 30.7 ms；不足窗口单列。

```bash
python3 examples/LonghaulCC/run-longhaul-all.py --algorithms dcqcn proposed \
  --flow-files examples/LonghaulCC/flow-longhaul-s2-scaled.txt \
               examples/LonghaulCC/flow-longhaul-s3-scaled.txt \
               examples/LonghaulCC/flow-longhaul-s5-scaled.txt \
  --purpose transient --stop-times 0.18 --output-root /tmp/longhaul-scaled

# R4，以及无本地竞争对照（复用历史输入文件）
python3 examples/LonghaulCC/run-longhaul.py --algorithm proposed \
  --flow-files examples/LonghaulCC/flow-longhaul-r3-mechanism-control.txt \
               examples/LonghaulCC/flow-longhaul-r3-mechanism.txt \
  --queue-mode history --service-mode busy-observed --stop-times 0.18 --output-root /tmp/r4-history

# 只替换为旧快照队列；B 反应、预算、整形、失效条件完全相同
python3 examples/LonghaulCC/run-longhaul.py --algorithm proposed \
  --flow-files examples/LonghaulCC/flow-longhaul-r3-mechanism.txt \
  --queue-mode snapshot --service-mode busy-observed --stop-times 0.18 --output-root /tmp/r4-snapshot
```

已按实际 ECMP 验证机制路径共同经过 `81→79`，随后分别到 Leaf73/Leaf75；
本地流只与 `0→41` 共享 `73→41`。仍可能通过共享源端、上游队列或 PFC 相互影响。
不能把 B 看到的 CNP 自动归因于 B 内部。

runner 默认构建；`--skip-build` 要求当前源码、二进制与共享库哈希匹配上次 runner 构建记录。
每次运行保留有效 `config.txt`、`inputs/` 输入副本、源码归档、Git HEAD 和未提交差异、
二进制/共享库哈希、命令、用途和窗口。有效配置引用输入副本，不覆盖历史结果。
输出仍为 `results/longhaul-YYMMDD-HHMM/<scenario>/<algorithm>/seedconfig-runN/`。
相同 seed/run 的重复不自动等于独立样本；本轮不进行大规模扫描或全面性能矩阵。

C++ CLI 保持 `--conf`、`--cc`、`--flow-file`、`--stop-time`、`--seed`、`--run`。
旧快照开关为配置项 `PROPOSED_RECONSTRUCT 0`，默认 `1`，runner 的模式参数写入有效配置。
R4 默认显式配置 `PROPOSED_ECN_MODE source-boundary`，A/B 注册受控出口均不新增 CE；
此模式拒绝显式设置旧 `PROPOSED_SHAPER_ECN` 键。只有显式 `path` 模式允许该键：
`1` 允许 A/B 受控出口按原规则标记，`0` 同时禁止其新增标记，且不清除已有 CE。
普通交换机 ECN、STATE 和 PFC 仍保留；这项旧开关不能替代 A 的 CE 转近端反馈边界。
历史 R3 的关闭整形标记候选在两个机制场景 seed 改善跨域 FCT，但历史重建未显示稳定增量收益，
且 seed 1 仍慢于 DCQCN；配置、对照和负结果见 [R3 实测改进记录](R3_EVALUATION_20260929.md)。

## 5. 公共测量与分析

```bash
python3 examples/LonghaulCC/analyze-longhaul.py --root /tmp/longhaul-scaled
python3 examples/LonghaulCC/plot-longhaul.py --root /tmp/longhaul-scaled --all-scenarios
```

所有算法新增相同的公共记录，由 `longhaul-measurements.h` 实现：

- `metadata.json` 的 `flow_path_metrics` 包含实际有向 data/ACK 路径及容量，包括本地流。
- `metadata.json.flow-state.csv`：实际发送序号高水位、TX payload、唯一 RX payload、首次供给结束及完成时刻。
  TX 包含重传；供给结束取真实发送事件，不能用 ACK 尚未返回代替持续需求。
- `metadata.json.queues.csv`：路径端口/PG 的队列与暂停采样；WAN、B 向内及其他端口有独立坐标。
- `metadata.json.summary.json`：所有算法的预期/完成流数、逐流最终字节、全网准入丢弃及停止残留。
  `remaining_mmu_bytes`、`remaining_mmu_egress_bytes` 额外检查 MMU 总量与出口记账是否清空。
  Proposed 另保留 `r4.summary.json` 同口径副本。
- `buffer_resources`：每交换机共享池、逐端口每 PG headroom 及合计池大小。

分析输出：

| 文件 | 口径 |
|---|---|
| `flow-summary.csv` | 每条流均保留，含未完成、重复 FCT、RX 残留及重传 payload |
| `summary.csv` | 持续供给集合变化后的逐流阶段，参考目标、收敛/未收敛/不足窗口/不足样本 |
| `run-summary.csv` | 未完成数量、资源门槛、可评估阶段数、未收敛比例；收敛中位数仅为已收敛子集 |
| `direction-summary.csv` | A→B/B→A 分开，整段及预先定义窗口；队列最大值明确为采样最大值 |
| 每运行的 `queue-window-summary.csv` | 按节点/端口/PG 分开，不合并 WAN、网关内向与其他队列 |
| `scenario-validation.json` | 加入/退出/背景重叠与机制路径关系，失败明确保留 |
| `r4-evidence.csv` | 按 node/role/flow/generation 和注册 peer 关联，终点取该 ns 全部 B 事件之后；核对报告 ordinal、累计字节、固定前向时延和预测误差 |
| `r3-evidence.csv`（历史） | 按组去重预测值、A 组积压、最终/端口分配前限额、暂停及实际发送；按 `t+df` 对齐未来 B 队列 |

公平参考采用有向链路约束下等权 max-min **payload** 速率，计入数据头和反向 ACK 的
平均 wire 消耗；不包含重传、PFC/CNP/STATE 开销或窗口限制，不声称协议必然达到该参考。
缺少路径时标记 `unavailable_reference`，不退回同向均分。
供给结束后不再把该流作为持续需求流；此前已进入网络的在途/积压属于排空阶段。
旧数据缺少公共最终计数时，不自动认证其完成或持续供给状态。

预测误差只在有效快照下评估。R4 的 `r4-evidence.csv` 用 ADMIT/TX 事件重建模型终点
`t+df` 的 B 队列，并采用该纳秒全部事件后的边界；历史 R3 分析使用不早于终点的 B 样本，
保留采样时刻及采样偏差。不能拿 A 在 t 的预测直接与 B 在 t 的队列比较。
对比模式必须检查 A 当时有积压、实际出队确有变化及其他限制；无收益或效果变差也应保留。

## 6. 验收边界

构建与小型确定性校验：

```bash
./ns3 build longhaul-convergence longhaul-r4-test -j2
./ns3 run longhaul-r4-test --no-build
python3 examples/LonghaulCC/test-longhaul-analysis.py
```

`BUFFER_SIZE 50` 不是全系统总缓存：共享 50 MiB 之外还加入逐端口、逐 PG headroom。
当前 headroom 保留 `3×传播时延` 裕量并加入两端实际接收处理延迟；默认 WAN 每 PG
375,750,000 B、100 Gb/s 内部端口每 PG 431,250 B。总池和无损出口池均包含 headroom，
共享 ingress 池仍为 50 MiB。普通 PFC 在 MMU 要求暂停期间每半个暂停周期续发，
显式恢复时停止；Bifrost 专属端口/PG 沿用自身周期逻辑。
这些修改来自实测丢弃定位，不构成普遍无损保证。历史资源设置和逐轮失败/对照记录见
[R3 实测改进记录](R3_EVALUATION_20260929.md)，其他负载和 baseline 须在新资源条件下重新验收。

1.50 s 等停止时间只作候选，必须依完成与残留检查；不能只排名完成子集。
HPCC/TIMELY/Bifrost 还需各自在对应拥塞负载下验收，TIMELY 阈值不视作已标定。
功能通过、场景关系成立、计算触发、实际执行差异和性能收益是不同层次的证据。
历史场景修改记录见 [场景修改验证记录](EXPERIMENT_VALIDATION.md)；
R4 的实现核对及分版本验证记录见 [R4 实现与功能验证记录](R4_IMPLEMENTATION_VALIDATION.md)。

## Proposed R4（2026-10-08）

本轮实现及实际验证范围见 [R4 实现与功能验证记录](R4_IMPLEMENTATION_VALIDATION.md)。

本节对应 2026-10-09 工作区源码核对；本次未构建或运行测试。2026-10-08 的测试与实验
属于已有记录，不自动证明当前工作区或二进制已重新验证。

当前 `--cc=proposed` 运行 R4，入口为 `longhaul-r4.h`，必须显式配置 `PROPOSED_VERSION 4`。
使用 `config-longhaul-r4.txt`；旧 `config-longhaul.txt` 不静默迁移，旧 QREF、
SOURCE_HIGH/LOW、FALLBACK_FRACTION 需分别改成 PORT_QREF、SOURCE_PORT_HIGH/LOW、
BUFFER_FALLBACK_FRACTION。runner 的 `--r3-queue-mode` 用于 R4 会明确报错。

`DciGatewayNode` 持有逐流反应、STATE/DEMAND、服务窗口、发送历史和生命周期；
每个物理出口对所有流做一次带上限等权分配。无 Group/Assembly。
`longhaul-convergence.cc` 包含 `longhaul-r4.h`，在流调度注册后调用 `Proposed::Setup()`，
由真实 ECMP 数据与反馈路径注册两个网关，随后启动公共测量。网关 `Tick()` 依次关闭 B 服务窗口、
采样端口、更新需求、计算 B/A 控制、生成报告、记录决策、批量提交设备控制，再发报告与近端 CNP。
逐流完成回调只关闭源侧状态；B 通过 DEMAND 接收关闭标志。停止汇总由公共测量生成。
`QbbNetDevice` 在成功入队后报告 ADMIT，批量应用完整端口控制；从队列取出可发送报文后，
在 `QbbDequeue` 通知及链路发送之前扣减普通令牌，探测模式另扣逐流额度与端口共享探测令牌。
普通交换机、DCQCN 和 PG/PFC 保持原职责。

默认 `PROPOSED_STARTUP_MODE optimistic`：A 按本地/B 出口容量起步，B 首次活动从出口容量初始化，均受端口联合分配约束，不使用额外启动字节额度。
首次有效活动 STATE（sample>=start 且 HAS_DEMAND，包括活动零预算）接管后不再重新乐观启动；无活动反馈达到 start+STATE_TIMEOUT 停发。
`probe` 保留一次有限启动探测；启动前零预算 STATE 不跳过启动。失效/缺历史/暂停停止普通注入但保持控制报告；
恢复探测要求新鲜可发送状态、低积压和正预算，后续额度必须有 B 累计接收推进。
完成标志通过 DEMAND 传到 B，尾部继续排空，身份保留到仿真结束。
端口目标队列按预算比例分配；源侧高低水位按有需求流等分，不随流数复制总量。

`--queue-mode history|snapshot` 只改变控制使用的队列值，两者均维护、检查历史。
`--service-mode budget|busy-observed` 分别使用预算或窗口实际出队/积压墙钟时间；
busy-observed 取 `min(当前预算, 窗口实际出队字节/积压墙钟时间)`；积压时间包含暂停、令牌和调度等待，
不扣除暂停时间。当前暂停输出零，忙时不足回退预算。配置文件窗口 200 μs、最小忙时 50 μs；
省略这两项时，接线分别使用报告周期和控制周期。
必须显式配置 `PROPOSED_ECN_MODE source-boundary|path`。默认 source-boundary：A 将上游 CE 记录为近端 CNP 待反馈，并改写为 ECT(0)，保留 DSCP；A/B 受控出口禁止新标记，B 保留已有 CE，下游普通队列继续 ECN。B 的整形积压由 STATE→A 注入控制处理，不再通过自身标记降低自身预算。元数据 `ecn_policy_revision=2` 区分此前保留 B 标记的版本。上游 CE 与 A 积压共用限频，空队列/限频不会丢失待反馈。
正常边界模式要求 `PROPOSED_NEAR_CNP 1`。旧 `PROPOSED_SHAPER_ECN` 键只在显式 path 模式允许，保留原 A/B 同时开关语义；旧配置必须显式迁移，不能直接以其值 0 替代 A 边界。
当前 `RdmaHw::ReceiveUdp` 仍以非零 ECN 计拥塞并设置 ACK-CNP，A 改写为 ECT(0) 后仍可能
触发远端反馈。网关边界代码已实现，端到端源域反馈隔离尚未成立；`longhaul-r4-test`
中的网关/设备回归不覆盖主机接收端这项语义修正。
活动流瞬时空队列保留合法整形许可，新包按已有许可调度，不等待下一控制周期；暂停和令牌约束仍生效。
端口目标/高水位总和须小于共享缓存的 80%；应急保护采用 A/B 各自累计积压的
80%/50% 高低水位，仅作为安全机制，不是缓冲预留或无损证明。

控制报文为 version 4、16 B 公共头、152 B STATE 或 64 B DEMAND，按 IP MTU 分包，
每条记录独立校验应用。日志前缀为 `r4`：

- `r4.gateway-N.flows.csv`：逐流决策、服务窗口、状态有效性、探测模式和累计计数。
- `r4.gateway-N.ports.csv`：端口容量、目标和、实际受控/其他发送、共享探测桶。
- `r4.gateway-N.packets.csv`：ADMIT/TX 与纳秒事件序号。
- `r4.gateway-N.events.csv`：报告、拒绝原因、CNP、探测和保护事件。
- `r4.paths.csv`、`metadata.json`：路径、角色、代次、时延、参数和状态成本。

`plot-longhaul.py` 保留公共图，并为每个 R4 run 生成 `r4-control.png`；
目标/服务估计曲线不等同实际吞吐。完成验收同时检查主机 FCT/payload 和网关 wire 闭合。

迁移前源码快照在项目根目录 `results/r4-implementation-20261008/r3-before/`。
旧 R3 接线、测试和论文脚本仍保留；R3 测试不参与当前构建。
历史脚本需在 R3 快照环境运行，不把当前二进制标成 R3。

## Proposed R3 历史实现（2026-09-29，非当前运行入口）

以下描述只对应迁移前 R3 快照；其配置、实验结论和论文脚本保持历史含义。

R3 快照的 `--cc=proposed` 运行 R3，RNIC 保留 DCQCN。两个 DCI 创建为 `DciGatewayNode`，
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

历史 runner 将 R3 输出前缀隔离为每次运行目录下的 `r3`：

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
