# LonghaulCC 项目进度与下一步

更新时间：2026-10-02；路径更新：2026-10-08。范围：当前仓库的 `longhaul-convergence`、Proposed R3、相关实验和本机运行准备。

## 1. 现在做到哪一步了

**代码层面：已具备 LonghaulCC 实验平台、Proposed R3 控制器、场景输入和公共分析工具。研究层面：已有历史功能验证和机制对照记录，但尚未证明 R3 相对 DCQCN 的稳定性能优势。本机层面：已完成路径适配并生成 ns-3 构建配置，下一步是完成目标编译和本机运行验证。**

这里区分三种证据：

- **当前源码与文件检查**：说明现在具备什么功能、配置是什么。
- **历史实验文档**：说明此前记录过什么现象；本机没有对应原始结果，尚未重新复现。
- **本机实际验证**：仅记录本次工作中实际检查或执行成功的内容。

| 阶段 | 当前状态 | 尚缺什么 |
|---|---|---|
| 长距 RDMA 仿真平台 | 主程序、拓扑、流量、算法选择和测量入口已存在 | 本机目标编译和启动确认 |
| Proposed R3 实现 | A/B 网关控制、预算、历史重建、消息与整形已接入 | 本机控制器回归和机制场景复现 |
| 实验场景与统计整改 | 缩小场景、机制场景、完成验收和路径参考已在源码中实现 | 在当前机器生成新结果并核查 |
| 公共 PFC/缓存修复 | 出口池、暂停续期和处理时延 headroom 已在代码中实现 | 当前资源条件下各 baseline 的拥塞验收 |
| 本机路径与 Python 兼容 | 已修改，路径和哈希检查通过 | 修改尚未提交 |
| 本机构建环境 | CMake 已安装，optimized 配置已生成 | 未找到 LonghaulCC 可执行文件，无法确认目标编译完成 |
| 本机实验结果 | 未找到结果目录 | 先生成启动、完成和机制结果 |
| 性能研究结论 | 历史文档保留了改进与负结果 | 历史重建收益、baseline 标定及更广负载比较仍待验证 |

## 2. 本机快照与当前路径

仓库路径已按 2026-10-08 的工作目录更新。下表其余环境、构建和实验状态是 2026-10-02 的记录，尚未在当前环境重新核实。

| 项目 | 检查结果 |
|---|---|
| 仓库目录 | `/home/shemuping/newCode/ns3-FRP` |
| ns-3 目录 | `/home/shemuping/newCode/ns3-FRP/simulator/ns-3.39` |
| 当时分支 | `codex/longhaul-local` |
| 当时提交 | `1773c92`，提交说明为“改善pfc配置” |
| Python | 3.10.12 |
| CMake | 3.22.1，现已安装；此前“缺少 CMake”的阻塞已解除 |
| 编译工具 | `g++`、Make 可用；未找到 Ninja，可使用 Make |
| ns-3 配置 | `.lock-ns3_linux_build` 和 `ns3config.txt` 均记录 optimized，examples 开启，tests 关闭 |
| Python bindings | 缓存中请求开启，但配置摘要显示依赖不足、实际未启用；LonghaulCC 无需此功能 |
| 绘图依赖 | 当前 Python 未找到 `matplotlib` |
| 主程序产物 | 未找到 `build/examples/LonghaulCC/ns3.39-longhaul-convergence-optimized` |
| runner 构建记录 | 已取消；`--skip-build` 仅跳过编译并使用现有程序 |
| 仿真结果 | 项目根目录和 ns-3 根目录均未找到 `results/` |

存在 `build/`、`cmake-cache/` 和构建配置，只能确认配置步骤已经推进，不能据此判定目标编译或实验成功。本文整理期间没有重新构建或运行完整仿真。

### 本次已经修改的文件

| 文件 | 修改内容 |
|---|---|
| [run-longhaul.py](../simulator/ns-3.39/examples/LonghaulCC/run-longhaul.py) | 承接完整单算法执行流程；配置/流量参数相对路径先查启动目录，再查 ns-3 根目录；增加拓扑文件检查；保留兼容 Python 3.10 的输入哈希，取消 Git 信息、源码/构建一致性检查和输入复制 |
| [run-longhaul-all.py](../simulator/ns-3.39/examples/LonghaulCC/run-longhaul-all.py) | 逐个调用单算法脚本，统一结果根目录，首次构建后复用现有程序，汇总失败算法 |
| [README.md](../simulator/ns-3.39/examples/LonghaulCC/README.md) | 更新本机目录，说明路径规则、首次配置和结果位置；根据当前配置将默认场景说明改为 S0 |
| [LONGHAUL_USER_GUIDE.md](../simulator/ns-3.39/examples/LonghaulCC/LONGHAUL_USER_GUIDE.md) | 同步默认场景为 S0 |

上述四个文件当前仍为未提交修改；本文是新增的第五个文件。未修改 R3 控制公式、流量输入或算法参数。

## 3. 代码怎么组织、从哪里运行

主要执行顺序：

```text
run-longhaul-all.py（批量时逐个调用单算法入口）
  → run-longhaul.py（单算法也可直接从此开始）
  → 构建目标、引用原始输入、生成有效 config.txt
  → longhaul-convergence.cc 创建网络、流和公共测量
  → proposed 模式接入 longhaul-r3.h 与 DciGatewayNode
  → 生成仿真 CSV、汇总和 runner 元数据
  → analyze-longhaul.py 分析
  → plot-longhaul.py 绘图
```

| 代码位置 | 职责 |
|---|---|
| [longhaul-convergence.cc](../simulator/ns-3.39/examples/LonghaulCC/longhaul-convergence.cc) | 主程序：读取配置/拓扑/流，设置算法、路由、资源和测量 |
| [config-longhaul.txt](../simulator/ns-3.39/examples/LonghaulCC/config-longhaul.txt) | 公共配置；当前默认 S0、DCQCN、停止时间 0.38 s |
| [topology-longhaul.txt](../simulator/ns-3.39/examples/LonghaulCC/topology-longhaul.txt) | 两个 DC，共 82 节点、64 hosts；DC 内 100 Gb/s，DCI 200 Gb/s、单向 5 ms |
| `examples/LonghaulCC/flow-longhaul-*.txt` | 正式 S0–S5、缩小的事件场景及有/无本地竞争的机制场景 |
| [experiment-scenarios.json](../simulator/ns-3.39/examples/LonghaulCC/experiment-scenarios.json) | 预定义观察窗口和加入、退出、共存、机制路径等验收条件 |
| [longhaul-r3.h](../simulator/ns-3.39/examples/LonghaulCC/longhaul-r3.h) | 真实路径核验、流注册、R3 配置与输出接线 |
| [dci-gateway-node.cc](../simulator/ns-3.39/src/point-to-point/model/dci-gateway-node.cc) | R3 控制核心：B 反馈/预算、A 重建/限速、STATE/DEMAND 消息 |
| [qbb-net-device.cc](../simulator/ns-3.39/src/point-to-point/model/qbb-net-device.cc) | RDMA 设备、实际发送、PFC 与整形接入 |
| [longhaul-measurements.h](../simulator/ns-3.39/examples/LonghaulCC/longhaul-measurements.h) | 各算法公共路径、逐流字节、队列、缓存与最终状态记录 |
| [analyze-longhaul.py](../simulator/ns-3.39/examples/LonghaulCC/analyze-longhaul.py) | 完成状态、基于路径的公平参考、阶段收敛、方向和机制证据分析 |
| [plot-longhaul.py](../simulator/ns-3.39/examples/LonghaulCC/plot-longhaul.py) | 从仿真和分析结果绘图；需要 matplotlib |
| [longhaul-r3-test.cc](../simulator/ns-3.39/examples/LonghaulCC/longhaul-r3-test.cc) | 控制器、队列、消息、暂停/恢复等确定性回归 |
| [test-longhaul-analysis.py](../simulator/ns-3.39/examples/LonghaulCC/test-longhaul-analysis.py) | 路径容量、公平参考、收敛状态和汇总的 7 项分析测试 |

上表中未写完整前缀的源码路径以 `simulator/ns-3.39/` 为基准。

**算法入口要区分：**C++ 主程序接受 `dcqcn/hpcc/timely/bifrost/frp/rocc/proposed`，但两个 Python runner 当前仅开放 `dcqcn/hpcc/timely/bifrost/proposed` 五项。

### 路径与输出规则

- 项目和 ns-3 根目录由 runner 根据脚本位置自动定位，无需写死本机目录。
- 配置中的 `TOPOLOGY_FILE`、`FLOW_FILE` 相对 ns-3 根目录解析，现有值可直接使用。
- runner 会为每次运行改写输出路径，默认保存到项目根目录的 `results/longhaul-时间/场景/算法/seedconfig-runN/`。
- 自定义 `--output-root` 若使用相对路径，以启动目录为基准。
- 直接执行 `./ns3 run` 时，默认配置的输出在 ns-3 根目录下的 `results/longhaul/`，需要先建目录；建议优先使用 runner 保存实验来源。
- `run_scripts/` 是其他旧程序的入口，其中仍有旧绝对路径，当前 LonghaulCC 执行链不经过它们。

## 4. 文档怎么读、哪些属于历史

| 文档 | 用途与阅读顺序 |
|---|---|
| 本文 | 先看当前进度、本机状态和下一步 |
| [LonghaulCC README](../simulator/ns-3.39/examples/LonghaulCC/README.md) | 当前运行方法、配置、输出和验收边界，以此作为操作入口 |
| [使用手册](../simulator/ns-3.39/examples/LonghaulCC/LONGHAUL_USER_GUIDE.md) | README 的简要索引 |
| [R3 实现说明](../simulator/ns-3.39/examples/LonghaulCC/PROPOSED_R3_IMPLEMENTATION.md) | 当前机制、配置单位、实现范围和历史功能检查 |
| [场景修改验证记录](../simulator/ns-3.39/examples/LonghaulCC/EXPERIMENT_VALIDATION.md) | 2026-09-29 场景/测量整改过程；早期丢弃问题另有后续修订 |
| [R3 实测改进记录](../simulator/ns-3.39/examples/LonghaulCC/R3_EVALUATION_20260929.md) | 后续资源/PFC 修复、ECN 候选、FCT 对照和预测反例，研究现状重点看此文 |
| [场景 Review](LonghaulCC_Proposed_R3_实验场景Review_20260929.md)、[场景修改方案](LonghaulCC_Proposed_R3_实验场景修改方案_20260929.md) | 更早的检查和计划；部分“待实施”项目已出现在当前源码中，不能据此重复判定为未实现 |
| [早期基线计划](../simulator/ns-3.39/examples/LonghaulCC/LONG_HAUL_BASELINE_PLAN.md) | 研究背景；算法范围、配置名称和统计方法部分已被后续实现更新 |
| [docs/记录.md](记录.md) | 通用编译笔记；本机命令以当前 LonghaulCC README 为准 |
| `docs/旧版本文件/` | R1/R2 及早期设计、参考材料，保留历史用途 |

本次只新增进度与索引说明，不搬动文件或改写历史实验结论。

## 5. 研究上已完成什么、还没证明什么

以下实验现象来自仓库历史文档，本次未重新读取对应原始 CSV；本机当前没有这些结果目录。

### 已有实现和历史记录

1. R3 已从早期方案推进到专门的 A/B 网关实现：B 合并 CNP 反应并分配逐流预算，A 根据实际发送历史重建远端积压并执行排空限速；RNIC 保留 DCQCN。
2. 已加入 `history/snapshot` 对照。`PROPOSED_RECONSTRUCT=1` 使用历史重建，`0` 使用有效旧快照队列，保留其他控制和失效条件。
3. 已加入 S2/S3/S5 缩小场景及有/无本地竞争的机制场景，用于检查加入、退出、共存和不同下游竞争关系。
4. 已将未完成、未收敛、观察窗口不足和样本不足分开，并根据真实有向路径计算公平参考。
5. 已修复历史记录定位出的无损出口池、普通 PFC 续期和 headroom 接收处理时延问题。
6. 历史实测文档报告：修正资源后的机制完成对照可达到 3/3 完成、零准入丢弃、无 payload 重传、字节闭合以及停止时队列/MMU 清空。

### 当前主要研究缺口

- **历史重建没有稳定增量收益。**历史文档中已有预测改变实际发送的证据，但 history 相对 snapshot 的 FCT 改善不稳定，不能声称预测模块已经带来稳定收益。
- **服务估计模型需要进一步核查。**历史反例中预测约 28.45 MB，而对齐的未来 B 队列为 0；源码仍将快照服务估计用于后续推演。瞬时积压集合变化是需要验证的误差来源，尚不是本机已复现的根因结论。
- **ECN 候选不等于默认行为。**`PROPOSED_SHAPER_ECN=0` 在历史机制场景改善了跨域 FCT，但本地竞争流变慢，seed 1 仍慢于 DCQCN。当前默认仍为 `1`。
- **反馈不能自动定位拥塞来源。**B 看见的 CNP 包括路径拥塞反馈，尚未完全隔离上游 ECN，不能把全部反馈解释为 B 内部下游瓶颈。
- **尚缺完整的拥塞性能比较。**HPCC/TIMELY/Bifrost 的短流功能记录不能代替公共资源修复后的拥塞标定；正式负载、多种 seed 和更广场景仍待验收。

研究下一步优先顺序应是：本机复现 → 完整性验收 → 定位预测服务估计与反馈来源 → 单因素对照 → 扩展算法性能比较。

## 6. 接下来按什么顺序做

以下命令是后续操作步骤，本次整理未执行构建、仿真或依赖安装。每次实验用新目录，避免覆盖旧结果。

### 第一步：完成本机目标编译与基础回归

当前已有 optimized 配置，可以先直接编译目标：

```bash
cd /home/shemuping/newCode/ns3-FRP/simulator/ns-3.39
./ns3 build longhaul-convergence longhaul-r3-test -j2
./ns3 run longhaul-r3-test --no-build
python3 examples/LonghaulCC/test-longhaul-analysis.py
```

若需要重新生成配置，使用 README 中的 optimized 命令并关闭不需要的 Python bindings。runner 当前按 optimized 文件名寻找主程序，切换到其他构建 profile 后需核对入口。

完成标准：两个目标编译成功，控制器回归退出码为 0，7 项分析测试通过。当前只有分析测试已在本机通过；编译和 C++ 回归仍待确认。

### 第二步：跑 DCQCN 与 R3 的短时启动检查

在同一个 ns-3 根目录执行：

```bash
python3 examples/LonghaulCC/run-longhaul-all.py \
  --algorithms dcqcn proposed \
  --flow-files examples/LonghaulCC/flow-longhaul-s0.txt \
  --purpose transient --stop-times 0.03 \
  --output-root /home/shemuping/newCode/ns3-FRP/results/local-s0-smoke-20261002
```

完成标准：两个运行的 `runner-metadata.json` 均为 `status=ok`，有效配置引用原始输入，配置和测量输出存在。S0 是 3 GB 流，0.03 s 只用于启动检查，`completion_valid=false` 不代表启动失败。

首次运行不要加 `--skip-build`；该选项仅跳过编译，要求主程序已存在。修改 C++ 代码后需重新构建。

### 第三步：确认完成验收与分析链

先运行 S0 完成实验，统一选 1.50 s 作为候选停止时间：

```bash
python3 examples/LonghaulCC/run-longhaul-all.py \
  --algorithms dcqcn proposed \
  --flow-files examples/LonghaulCC/flow-longhaul-s0.txt \
  --purpose completion --stop-times 1.50 \
  --output-root /home/shemuping/newCode/ns3-FRP/results/local-s0-completion-20261002

python3 examples/LonghaulCC/analyze-longhaul.py \
  --root /home/shemuping/newCode/ns3-FRP/results/local-s0-completion-20261002
```

1.50 s 尚未在本机验证为充分时长；若有未完成，先检查日志与原因，必要时给两算法统一延长后使用新目录重跑。

完成标准：`completion_valid=true`，预期流数与唯一 FCT 行数一致，逐流有效 RX 字节等于输入大小。另查 `metadata.json.summary.json` 中的丢弃、TX/RX、重传和停止残留；runner 的完成标志本身不自动保证零丢弃或队列清空。

分析应生成 `run-summary.csv`、`flow-summary.csv`、`summary.csv` 等。绘图前安装当前 Python 可用的 matplotlib，再按 README 执行 `plot-longhaul.py`；未安装时仍可先完成 CSV 分析。

### 第四步：复现场景关系和 R3 机制

先运行 DCQCN/R3 的 S2/S3/S5 scaled 输入，统一使用 0.18 s 瞬态设置，检查 `scenario-validation.json` 中的加入、退出及背景/探测重叠关系。

随后最小对照包括：

| 对照 | 要回答的问题 |
|---|---|
| 有本地竞争的 DCQCN | 当前公共资源下的基线表现 |
| 无本地竞争的 R3 history | 没有新增下游竞争时是否仍出现异常反应 |
| 有本地竞争的 R3 history | B 反应、A 预测与限速是否在有待发数据时改变实际发送 |
| 有本地竞争的 R3 snapshot | 历史重建相对旧快照的增量作用 |

历史机制完成对照使用 8 s，可作为本机复现候选。各对照保持对应输入、公共资源、seed/run、停止规则和窗口一致；有/无竞争仅改变明确的竞争流因素。先验收完成、字节、丢弃和残留，再比较 FCT 或收敛。

若复现 ECN 候选，应从当前公共配置复制独立配置文件，仅将 `PROPOSED_SHAPER_ECN` 改为 `0`，并保存该配置。历史文档中的结果目录和候选配置没有随当前下载保留，不能直接使用其旧路径。

### 第五步：处理预测问题，再扩大比较

1. 用新数据核对历史预测反例：快照积压、逐流预算、实际 B 出队、A 历史发送和 `t+df` 对齐的未来 B 队列。
2. 区分服务组成变化、暂停和采样对齐造成的误差，再确定最小模型修改；不要仅靠调整 `qref/tau` 避开反例。
3. 保留 history/snapshot 对照和负结果，确认预测变化实际影响有积压时的发送。
4. 在公共资源和统计口径固定后，验收 HPCC/TIMELY/Bifrost 的拥塞表现，再扩展正式 S0–S5、多 seed 和需要研究的新负载。

## 7. 本次验证与后续记录要求

本次路径适配阶段已完成：Python 语法检查、Python 3.10 的 SHA-256 一致性检查、多个启动目录的输入路径解析，以及缺失拓扑提前报错检查。本次进度整理又实际运行了分析测试，结果为 **7/7 通过**。

尚未本机验证：LonghaulCC 编译、C++ 控制器回归、任何完整仿真、绘图以及历史性能结果复现。没有原始结果支持的历史结论，本文均作为文档记录引用。

每推进一步，在本文或新的实验记录中补充：运行目录、实际命令、有效配置、算法、seed/run、停止时间、验收结果及失败原因。runner 保留输入路径及哈希；Git 信息按需手动记录，失败实验也保留，不只记录成功子集。

完成本机第一轮验证后，再提交当前路径修改和本文到 `codex/longhaul-local`。本次未自动提交、安装依赖或扩大实验矩阵。
