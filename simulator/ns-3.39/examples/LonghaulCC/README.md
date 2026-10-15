# LonghaulCC

入口为 `longhaul-convergence.cc`。当前 `--cc=proposed` 实现 R5 方案一，主机使用原生 DCQCN。
C++ 还支持 `dcqcn/hpcc/timely/bifrost/frp/rocc`；通用 runner 提供前四种基线和 Proposed。

## 运行

命令在 `simulator/ns-3.39` 下执行：

```bash
./ns3 build longhaul-convergence longhaul-r5-test -j2
./ns3 run longhaul-r5-test --no-build
python3 examples/LonghaulCC/test-longhaul-analysis.py
python3 examples/LonghaulCC/run-longhaul.py --algorithm proposed \
  --flow-files /absolute/path/to/finite-flow.txt --stop-times 0.1 \
  --purpose completion --output-root /absolute/path/to/new-results
python3 examples/LonghaulCC/analyze-longhaul.py --root /absolute/path/to/new-results
python3 examples/LonghaulCC/verify-longhaul-r5.py --root /absolute/path/to/new-results
python3 examples/LonghaulCC/plot-longhaul.py --root /absolute/path/to/new-results --all-scenarios
```

`run-longhaul-all.py --algorithms dcqcn hpcc timely bifrost proposed` 将其他参数传给单算法 runner。
Bifrost 当前仅接受 PG=3。不要将包含其他 PG 的输入作为 Bifrost 功能验收。
`verify-longhaul-r5.py` 用于有限流完成验收；瞬态运行允许尚未完成，不能套用此验收条件。

默认读取 `config-longhaul.txt`，当前是 S1、Proposed、0.2 s。算法参数在 C++：
主机/基线参数位于入口常量区，R5 参数位于 `DciGatewayNode::Config`。
配置文件只保存实验设置，`PROPOSED_OUTPUT` 指定日志前缀。
旧 Proposed 版本、ECN 模式、DEMAND、探测额度及重建开关不再存在。

流文件第一行为流数，其余每行：

```text
src dst pg dport size_bytes start_time_seconds
```

源 UDP 端口由入口按源/目的主机对分配；R5 仅获取连接身份和真实路由，不获取流大小、
起始时间或完成通知。S0–S5 和 scaled 输入保留；已由用户删除的 R3 mechanism 输入不再作为可运行示例。

runner 将配置与输入复制到运行目录，通过 `--config` 传给 C++，记录输入/配置/源码/二进制/
共享库哈希和完整命令。使用新结果目录；`--skip-build` 只有在当前身份与上次构建记录一致时可用。
相对输入路径先相对调用目录查找，再相对 ns-3 根目录查找。

## R5 模块与边界

- `longhaul-routing.h`：所有算法共用的真实 ECMP 路由和时延检查。
- `longhaul-r5.h`：管理路径、控制域与发送端 QP 关联，以及参数/终态元数据。
- `DciGatewayNode`：B 控制域测量、独立状态报文、A 逐包实际发送重放、终点控制、峰值保护、局部 CNP。
- `QbbNetDevice`：共享组级令牌桶和原有逻辑队列轮询。空流不占份额，所有有数据流受同一组额度约束。
- `SwitchNode/BEgressQueue/SwitchMmu`：普通转发、ECN、PG、公用缓存和 PFC。B 没有附加整形。

控制域是 B 的真实出口端口与 PG。同一目的主机的不同流可能经 ECMP 落到不同控制域，
以 `r5.paths.csv` 为准。基本实现为单对直连网关、固定管理路径；组内只接受同一个 A。
路径映射改变时停止仿真并要求重新关联，不能让旧预测继续控制新的资源。
不实现多入口协调、下游独立限速或短/长流分类。

A 实际出队才记录发送历史；到达时刻包含实际报文长度对应的串行化时间、WAN 传播和 B 接收处理。
控制更新在当前仿真事件原子应用（执行延迟取 0），已经出队的报文留在历史中。
B 空闲不意味着容量降低；全区间未排空、未暂停的共享服务才用出队速率更新有效服务估计。
未知竞争/未来暂停仍是估计误差。服务明确停止时禁止用目标队列项补发。

保存决策当时的预测，离线按 B 的真实逐包事件评价终点与中途峰值。另保存尚未发生的下一次
周期采样预测，收到对应反馈后计算残差；连续三次超过余量时降到保守状态。
缓冲预算为配置池的 80% 等分，进一步受所有出口当前占用影响；不将 PFC headroom 当作重复可用空间。
这是一套有限资源下的风险控制，不是独占缓存预留或任意扰动下零丢包保证。

原生 CE、CNP 和 ACK-CNP 原样透传；普通 ECN 继续生效。近端 CNP 仅依据本地缓存压力，
通过源 IP、源 UDP 端口和 PG 定向到发送 QP。组内轮询为报文级公平；不同包长的严格字节公平未实现。

## 输出与验收

所有算法共用 `sender-rate.csv`、`receiver-goodput.csv`、`fct.csv`、`measured-rtt.csv`、
`dci-link.csv`、`pfc.csv`、`metadata.json` 及 `metadata.json.summary.json` 等测量。
终态汇总包含逐流唯一发送、实际发送 payload、RX、完成和全网残留/MMU/准入丢弃。

R5 默认前缀为 `r5`：

| 文件 | 内容 |
|---|---|
| `r5.paths.csv` | 每条跨域连接的实际路径与 B 出口 |
| `r5.gateway-N.groups.csv` | 组级队列、服务、反馈边界、在线预测、峰值与速率指令 |
| `r5.gateway-N.packets.csv` | 逐流实际 ADMIT/TX、完整报文长度、预测到达时刻 |
| `r5.gateway-N.group-packets.csv` | 全组（含非受控流量）ADMIT/TX/DROP 与事件顺序 |
| `r5.gateway-N.events.csv` | 实际应用的 CONTROL、STATE、CNP、预测残差与拒绝 |
| `r5.final.json` | 两端逐组/逐流累计计数和反馈计数 |
| `r5.summary.json` | 公共终态汇总的同口径副本 |
| `r5-evidence.csv` | 分析器输出：逐包到达映射、快照边界、在线预测与之后的真实队列比较 |
| `r5-validation.json` | 验收器输出：完成/字节/丢弃/残留与共享令牌支出的核对 |

先分析再绘图。图中的 target/service 是指令和估计，不是实际吞吐。
PFC 按合并暂停区间、时长和覆盖范围评价，不能用刷新包行数代替拥塞次数。
构建通过、CSV 生成、完整流完成只支持相应功能结论，不支持性能改善结论。
实现对应关系和本轮验证见 [R5 实现与验证](R5_IMPLEMENTATION_VALIDATION.md)。
