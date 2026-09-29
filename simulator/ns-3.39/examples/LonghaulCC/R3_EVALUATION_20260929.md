# R3 实测改进记录（2026-09-29）

本轮实际构建、运行、读取原始结果，并逐轮只改变一个主要因素。
结论：修复了公共无损资源配置和 PFC 续期问题；一个可选的网关 ECN 候选明显改善了
本机制场景，但历史重建相对旧快照没有稳定收益，仍不能声称 R3 优于 DCQCN。

## 1. 来源、执行路径与实验合同

- 起点 HEAD：`629e1f678e443962a4f264b5b9c80aa6fdee925b`；开始时工作区干净。
- 核对了 `AGENTS.md`、入口、R3 接线/网关、队列/MMU/PFC、配置、runner、分析工具、
  R3 DOCX 讨论稿、实现说明、场景 Review 和修改方案。旧文档的“待实施”状态不覆盖当前源码。
- `run-longhaul.py` → `run-longhaul-all.py` → `longhaul-convergence.cc` 建网与配置 →
  `longhaul-r3.h` 注册真实路径 → `DciGatewayNode` 的 A/B 控制 → Qbb 实际出队与公共测量 →
  `analyze-longhaul.py`。实际路径仍是 host→leaf→spine→DCI，无直连假设。
- 原默认设置仍是 S2/DCQCN/0.38 s 瞬态；本轮显式选择 R3 机制场景。
  两条跨域流 `0→41`、`0→59` 各 750 MB，于 10 ms 启动；本地 `42→41` 的 500 MB 于 30 ms 加入。
  两条跨域流共用 `81→79`，本地流仅共享目标 `73→41`；真实路径由输出再次验证。
- 固定观察窗沿用 `experiment-scenarios.json`：10–30 ms、30–80 ms。
  首轮 0.18 s 瞬态用于定位；完成对照统一使用 8 s，不按算法结果分别选停止时间。
  8 s 是按 750 MB 在 1 Gb/s 下约 6 s 加余量预先选的保守上限；实际均更早完成。
  尾部空闲时间不用于解释利用率、预测精度或公平性。
- `qref=500000 B`、`tau=0.02 s`、反应/恢复/报告周期、拓扑、流量和 RNIC DCQCN 参数未调优。
  单次完成运行约 38–45 s 墙钟时间；确认成本后只扩展到第二个配对 seed 和 S0/S4 功能检查。

结果根目录（相对 ns-3.39）：

`results/r3-evaluation-20260929-173441/`

每个运行目录保存有效 `config.txt`、流/拓扑副本、日志、元数据、源码/二进制/库哈希与 CSV。
根目录的 `round1.patch`、`round2.patch`、`round3.patch`、`round4b.patch`、`final-code.patch`
保存相对起点 HEAD 的累积代码差异。所有运行使用新目录，未覆盖旧结果。
`evaluation-summary.json` 由同目录 `summarize.py` 从原始记录重建；各阶段保留现有分析工具的输出。

## 2. 逐轮定位与单因素改动

### 第一轮：无损出口池不能使用已经配置的 headroom

**已观测：**`baseline-history/` 完成 1/3，全部 32,404 个 admission drops 来自出口准入。
B 队列峰值 52,428,296 B，接近 50 MiB 出口池上限；并非 ingress headroom 耗尽。

**实现/配置缺陷：**总池为共享池加 headroom，无损出口池却仍只有共享池大小。
同一报文同时受 ingress/egress 准入检查，导致已配置的 headroom 无法通过出口准入使用。

**改动：**`SetEgressLosslessPool(shared + totalHeadroom)`；共享池与每 PG headroom 保持不变。

**对照：**`egress-pool-history/` 同为 0.18 s，drops 从 32,404 降为 0；逐流 RX 没有增加，
B 峰值增至 86,387,688 B。因此这是准入完整性改善，不是吞吐收益。
同资源下旧快照和无竞争对照分别保存在 `egress-pool-snapshot/`、`egress-pool-control/`。

### 第二轮：普通 PFC 没有续期

**已观测：**延长至 8 s 的 `completion-history/` 虽然完成 3/3，却有 573 个 ingress drops，
流 1 重传 payload 48,598,000 B。丢弃出现在首次暂停之后数毫秒，不能只归因于传播在途数据。

**实现缺陷：**Qbb 接收方在配置的 5 μs 暂停到期后恢复；MMU 仍保持 paused 状态，
`CheckShouldPause()` 因此不再发送暂停，直到排空后显式 resume。

**改动：**普通 PFC 每半个暂停周期续发，直到显式 resume；resume/dispose 取消计时器。
Bifrost 专属端口/PG 继续由其原有定时器处理。

**对照：**`pfc-refresh-history/` 中旧的长时间暂停失效阶段消失，但恢复发送后的新暂停发生
626 个 ingress drops，流 1 重传 payload 48,834,000 B。修复没有被写成无丢包或性能提升。
`pfc-refresh-snapshot/` 保存同条件旧快照对照。

### 第三轮：headroom 缺少实际接收处理时间

**已观测：**恢复后的新暂停于约 392.480 ms 发出，其后短时丢弃，日志显示
`headroom 55544 xoff 56250 pktSize 1048`。

**实验资源缺陷：**内部链路的 56,250 B 只对应 100 Gb/s × 4.5 μs。
`QbbNetDevice::Receive()` 对数据与 PFC 均施加 15 μs 接收处理延迟，两端处理均影响停止输入的时间。

**改动：**保留原 `3 × propagation` 裕量，加入本端数据处理和对端暂停处理时间：

```
headroom = ceil(rate × (3 × propagation + local_receive_delay + peer_receive_delay) / 8)
```

默认内部链路每 PG 为 431,250 B，WAN 每 PG 为 375,750,000 B。
这仍是本模拟设置下的资源配置，不是任意排队、控制延迟或部署下的安全证明。
元数据现在显式记录 `egress_lossless_pool_bytes`。

**对照：**`headroom-history/`、`headroom-snapshot/`、`headroom-dcqcn/` 均 3/3 完成，
零 admission drop、无 payload 重传、停止队列清空；控制参数未改变。

### 第四轮：网关整形积压反馈到 B 逐流反应

**已观测：**有/无本地竞争，B 都在 25.45 ms 首次降速，B 当时组队列为 0，
且早于 30 ms 本地竞争；此前 A→B 已有 1,216 个 ECN 标记。
后续 B 整形积压也会被普通出口 ECN 标记。当前控制并未隔离标记来源。

**方案假设问题：**自身整形造成的排队被当成路径拥塞，反馈又降低整形器服务能力。
不能把这些 CNP 自动解释为远端下游拥塞。

**单因素候选：**新增 `PROPOSED_SHAPER_ECN`，默认 `1` 保留原行为；设为 `0` 时，
只在已注册 A/B 流的实际整形出口省略新增 ECN 标记。已有 ECN 位不清除，未注册流、
其他端口和普通交换机仍正常标记；RNIC DCQCN、近源 CNP、STATE、PFC 和水位保护保留。
这改变了端网反馈语义，是显式实验候选；不声称完成了所有上游标记隔离。

`no-shaper-*-v2/` 使用候选；`legacy-hook-check-v2/` 的 7,198 行 A 日志与旧行为同时间前缀逐行一致。
最初不带 `v2` 的四个目录因配置解析器不支持新增的 `#` 注释而在启动时失败；
已去掉注释并在新目录重跑，失败目录保留，不计为有效实验。

## 3. 完成结果与作用边界

FCT 单位 ms，均按完整流统计，包含本地竞争流；同一行全部流完成。
下表使用修正后的公共资源/PFC，每行 drops 和 payload 重传均为 0。

| seed | 配置 | 0→41 | 0→59 | 42→41 |
|---|---|---:|---:|---:|
| 1 | 原整形 ECN，history | 907.166 | 839.561 | 42.106 |
| 1 | 原整形 ECN，snapshot | 607.867 | 608.376 | 42.090 |
| 1 | 候选 ECN，history | 234.227 | 222.116 | 64.860 |
| 1 | 候选 ECN，snapshot | 240.202 | 217.301 | 64.976 |
| 1 | DCQCN | 144.728 | 122.875 | 90.985 |
| 2 | 原整形 ECN，history | 684.907 | 672.027 | 56.262 |
| 2 | 候选 ECN，history | 254.998 | 218.068 | 65.578 |
| 2 | 候选 ECN，snapshot | 231.601 | 221.696 | 65.557 |

**已观测：**候选改善了两个 seed 的跨域 FCT，但本地竞争流变慢，符合跨域流参与竞争增加的现象。
这不是所有流同时改善。seed 1 的候选 R3 仍慢于 DCQCN，不能宣称跨算法优势。
两个固定拓扑/负载的 seed 不足以估计普遍收益或置信区间。

seed 1 候选中，B 首次反应在 37.75 ms；流 1 有 18 次降速、流 2 为 0。
无竞争候选 `no-shaper-control-v2/` 中两流均无 B 降速，FCT 约 173.719 ms，B 峰值 1,048 B。
有竞争 history/snapshot 的 B 逐事件组峰值分别为 112,136 / 118,424 B。
机制路径和至少一个保持窗口的共同供给检查通过，history 共同供给延续至 94.794 ms。
这些对照支持本场景的异质反馈解释，但未单独消融 B 代理，不能独立归因全部收益于 B 逐流反应。

## 4. 历史重建：实际执行了，但收益不稳定

在预定义 30–80 ms 窗口，seed 1 两模式有 1,000 个同组有效配对样本；
188 个样本两边 A 均有积压，其中 43 个最终限额不同，且下一间隔实际 TX 也不同。
这是有待发数据时的执行差异；跨运行闭环状态不同，不能按单点直接作严格因果归因。

history 在该窗的平均绝对预测误差为 733,614 B（按 `t+df` 对齐的未来 B 采样），
不是在包含大量完成后空闲时间的 8 s 上平均。峰值反例：

- A 决策时刻 43.7 ms，快照时刻 38.65 ms，模型终点 48.715041 ms；比较 B 的 48.75 ms 样本。
- 快照积压 1,048 B，只有流 1 瞬时非空；两流各有 49 Gb/s 预算，但服务估计仅为 49 Gb/s。
- 预测积压 28,447,949.56 B，未来 B 采样积压为 0。
- A 当时仍有 62,880 B，最终限额 37.821 Gb/s，随后间隔实际发送 37.560 Gb/s。
- 快照至比较样本间，B 实际平均出队约 71.492 Gb/s。

**推断及依据：**把“瞬时积压集合的服务”在约 10 ms 推演区间保持常数，是本反例的重要误差来源；
后续流到达与服务组成发生变化。已有实际发送历史不能自动纠正错误的服务模型。
采样对齐误差约 35 μs，反例的巨大积压也远高于整次 B 峰值，不能仅用此采样偏移解释。
原始证据保存在 `prediction-counterexample.json` 和 `prediction-paired-evidence.json`。

seed 2 受影响流的 history FCT 比 snapshot 更差；保留负结果，不调整 `qref/tau` 制造收益。
下一项研究应针对服务组成变化设计可检验的估计，不能直接把所有预算相加替代服务，
否则会重新引入“积压集中于低速流”的高估问题。本轮未实现额外的逐流预测器。

## 5. 最终核验与复现

`final-history/`、`final-snapshot/` 补充 MMU 只读计数后重新完成同一 8 s 实验：

- 3/3 完成，FCT 无重复；两条跨域流各 750,000,000 B、本地流 500,000,000 B。
- 每流 unique TX、TX payload、unique RX 等于输入大小，无重传。
- 每条跨域流 A 入/出、B 入/出 wire 字节闭合；停止时逻辑队列为空。
- 全网 admission drops、交换机队列、MMU 总占用与 MMU 出口占用均为 0。
- FCT 与补充计数前对应候选逐字节一致，测量改动未改变该结果。
- 构建、R3 控制器确定性回归、PFC 续期/恢复回归和 7 项分析测试通过。
- `no-shaper-smoke/` 的 S0（1 流）、S4（16 流双向）1 MB 功能运行均完成且字节闭合；不视为持续拥塞性能证据。

在 ns-3.39 根目录运行（输出目录须新建）：

```bash
# 使用已保存的候选配置；公共默认 PROPOSED_SHAPER_ECN 仍为 1。
config=results/r3-evaluation-20260929-173441/no-shaper-ecn-v2.txt
python3 examples/LonghaulCC/run-longhaul.py --algorithm proposed --config "$config" \
  --flow-files examples/LonghaulCC/flow-longhaul-r3-mechanism.txt \
  --purpose completion --stop-times 8 --r3-queue-mode history \
  --output-root /tmp/r3-new-history
# 对照使用同一配置、流量、seed、stop-time，将 queue-mode 改为 snapshot 和新输出目录。
python3 examples/LonghaulCC/analyze-longhaul.py --root /tmp/r3-new-history
```

图表及复算脚本：结果根目录的 `r3-evaluation.png`、`r3-evaluation.pdf`、`plot-evaluation.py`。
图表分别显示 B 队列、逐流反应、对齐预测误差和跨域累计 RX；不是尾窗利用率排名。

本轮不扩大到完整 S0–S5 持续负载、incast、多 PG、更多 baseline 或参数扫描。
公共 PFC/资源修复会影响其他实验；HPCC/TIMELY/Bifrost 的拥塞性能需重新验收。
既有历史结果、DOCX 讨论稿和旧 Review 保留原时效，不改写成当前实验结论。
