# Workload 实验程序说明

本文档说明 `crossDC-evaluation-workload` workload 实验的程序流程、依赖文件、运行方式和输出文件。当前设计中，Python 脚本只负责选择算法、构造唯一输出文件名、build/run、解析日志和画图；实验参数主要由 `config-workload.txt` 控制。

## 1. 相关程序

### `run_single_workload_algo.py`

顶层运行脚本。它负责：

- 读取 `simulator/ns-3.39/examples/PowerTCP/config-workload.txt`。
- 从 config 中读取 `LOAD` 和输出文件模板。
- 根据算法名和负载生成唯一后缀，例如 `frp_load20pct`。
- 编译 `crossDC-evaluation-workload`。
- 运行 ns-3 可执行文件。
- 保存完整日志。
- 解析日志中的队列长度、吞吐、flow 数量等信息。
- 读取 FCT 文件并统计平均 FCT、P99 FCT。
- 读取 query flow FCT 文件并打印 query flow 统计。
- 生成结果图。

命令格式：

```bash
python3 run_single_workload_algo.py <ccMode> <algo_name>
```

常用例子：

```bash
python3 run_single_workload_algo.py 1 DCQCN
python3 run_single_workload_algo.py 3 HPCC
python3 run_single_workload_algo.py 7 TIMELY
python3 run_single_workload_algo.py 8 DCTCP
python3 run_single_workload_algo.py 13 FRP
python3 run_single_workload_algo.py 14 ROCC
```

可选参数：

```bash
--randomSeed 7
--timeout 180
```

### `crossDC-evaluation-workload.cc`

ns-3 C++ 仿真程序。它负责：

- 解析 `config-workload.txt`。
- 读取 topology、CDF、query flow 等输入文件。
- 创建网络拓扑、交换机、服务器和链路。
- 根据 CDF 和 `LOAD` 生成背景流。
- 根据 `QUERY_FLOW_FILE` 安装 query flows。
- 按 `ccMode` 运行不同拥塞控制算法。
- 输出 PFC、FCT、query flow FCT。
- 在日志中打印队列、吞吐和 workload 相关信息。

Python 运行 C++ 时只传这些关键命令行参数：

```text
--conf
--algorithm
--randomSeed
--fctOutputFile
--pfcOutputFile
--queryFlowFctFile
```

`LOAD`、`START_TIME`、`SIMULATOR_STOP_TIME`、`FLOW_LAUNCH_END_TIME`、`CDF_FILE_NAME`、`QUERY_FLOW_FILE` 等实验参数由 config 文件控制。

### `config-workload.txt`

workload 实验主配置文件。它负责控制：

- 拓扑文件路径。
- CDF 文件路径。
- 背景流负载。
- 仿真开始/结束时间。
- 背景流发起结束时间。
- query flow 文件路径。
- 输出文件模板。
- 拥塞控制和交换机参数。
- 队列监控参数。

## 2. 程序运行流程

一次运行的流程如下：

1. 执行 Python 脚本，例如：

```bash
python3 run_single_workload_algo.py 13 FRP
```

2. Python 读取：

```text
simulator/ns-3.39/examples/PowerTCP/config-workload.txt
```

3. Python 从 config 中读取 `LOAD`，例如：

```text
LOAD 0.2
```

4. Python 把负载转换成文件名后缀：

```text
0.2 -> 20pct
```

所以 FRP 在 0.2 负载下的后缀是：

```text
frp_load20pct
```

5. Python 根据 config 中的模板生成输出文件名：

```text
FCT_OUTPUT_TEMPLATE results/workload/fct/fct_{suffix}.txt
PFC_OUTPUT_TEMPLATE results/workload/pfc/pfc_{suffix}.txt
QUERY_FCT_OUTPUT_TEMPLATE results/workload/fct/query-flow-{suffix}.txt
```

生成：

```text
results/workload/fct/fct_frp_load20pct.txt
results/workload/pfc/pfc_frp_load20pct.txt
results/workload/fct/query-flow-frp_load20pct.txt
```

6. Python 编译 C++ 程序：

```bash
./ns3 build crossDC-evaluation-workload
```

7. Python 运行编译后的 ns-3 可执行文件，并传入算法和输出路径。

8. C++ 读取 config，安装拓扑、背景流和 query flows。

9. C++ 运行仿真并写出 FCT/PFC/query flow FCT。

10. Python 读取仿真日志和输出文件，打印统计结果并生成图片。

## 3. 输入文件

### 主配置文件

```text
simulator/ns-3.39/examples/PowerTCP/config-workload.txt
```

重要字段：

| 字段 | 作用 |
| --- | --- |
| `TOPOLOGY_FILE` | 拓扑文件路径 |
| `FLOW_FILE` | 原始 flow 文件路径，当前 workload 主要使用 CDF/query flow |
| `TRACE_FILE` | trace 输入路径 |
| `TRACE_OUTPUT_FILE` | trace 输出路径 |
| `FCT_OUTPUT_TEMPLATE` | 普通 flow FCT 输出模板 |
| `PFC_OUTPUT_TEMPLATE` | PFC 输出模板 |
| `QUERY_FCT_OUTPUT_TEMPLATE` | query flow FCT 输出模板 |
| `CDF_FILE_NAME` | 背景流大小分布 CDF 文件 |
| `LOAD` | 背景流负载 |
| `START_TIME` | 背景流开始时间 |
| `SIMULATOR_STOP_TIME` | 仿真结束时间，C++ 中直接赋给 `simulator_stop_time`，控制 `Simulator::Stop()` |
| `FLOW_LAUNCH_END_TIME` | 背景流发起结束时间 |
| `QUERY_FLOW_FILE` | query flow 输入文件 |
| `ENABLE_FLOW_FILE_BACKGROUND` | 是否生成背景流，当前控制 CDF 背景流开关 |
| `CC_MODE` | config 内算法模式，但实际单次运行由 Python 的 `ccMode` 覆盖 |
| `ENABLE_QLEN_MON` | 是否启用队列监控 |
| `QLEN_MON_START` / `QLEN_MON_END` | 队列监控时间窗口 |

注意：`BACKGROUND_FLOW_FILE` 当前会被 C++ 读取和打印，但实际背景流仍由 `InstallBackgroundWorkload(load, cdfTable, START_TIME, FLOW_LAUNCH_END_TIME, ...)` 使用 CDF 生成，并不是从该文件安装。

### 拓扑文件

```text
simulator/ns-3.39/examples/PowerTCP/topology.txt
```

该文件描述节点数量、交换机/服务器关系和链路信息。文件开头类似：

```text
106 42 16 169
32 33 34 ...
0 32 100000000000.0 1.5us 0
```

大致含义：

- 第一行：拓扑规模信息。
- 第二行：交换机节点列表。
- 后续行：链路两端、链路速率、链路时延、错误率或附加标志。

### CDF 文件

```text
simulator/ns-3.39/examples/PowerTCP/Alistorage.txt
```

用于背景流大小分布。格式类似：

```text
2000     0.0
3000     0.01
5000     0.03
...
10000000 1.0
```

每行通常表示：

```text
flow_size_bytes cumulative_probability
```

C++ 会根据这个分布随机生成背景流大小。

### Query Flow 文件

```text
simulator/ns-3.39/examples/PowerTCP/query-flow.txt
```

文件开头类似：

```text
12
0 8 3 20031 20000000 0.005
1 9 3 20032 20000000 0.005
```

格式：

```text
flow_count
src_node dst_node pg dport size_bytes start_time_seconds
```

其中：

- 第一行是 query flow 数量。
- 后续每行是一条 query flow。
- `src_node` / `dst_node` 是源/目的节点。
- `pg` 是 priority group。
- `dport` 是目的端口。
- `size_bytes` 是 flow 大小。
- `start_time_seconds` 是发起时间。

### 运行脚本

```text
run_single_workload_algo.py
```

这个文件本身也是实验入口。你通常只需要改 config 文件，然后运行不同算法命令。

## 4. 输出文件

假设运行：

```bash
python3 run_single_workload_algo.py 13 FRP
```

且 config 中：

```text
LOAD 0.2
```

则后缀为：

```text
frp_load20pct
```

### 日志文件

```text
dump/workload/algo_cc13_load20pct.log
```

内容包括：

- build 输出。
- C++ config 解析输出。
- workload 初始化信息。
- 背景流/query flow 数量。
- 队列长度日志。
- 吞吐日志。
- FCT/PFC 输出路径。
- 运行过程中的 warning 或 error。

如果仿真结果异常，优先查看这个文件。

### 普通 Flow FCT 文件

```text
results/workload/fct/fct_frp_load20pct.txt
```

由 C++ 写出，Python 会读取它统计：

- completed flows 数量。
- 平均 FCT。
- P99 FCT。

Python 当前按第 7 列读取 FCT ns 值。

### Query Flow FCT 文件

```text
results/workload/fct/query-flow-frp_load20pct.txt
```

文件头格式：

```text
# Query Flow FCT Results
# Format: src_node dst_node pg dport flow_size(B) start_time(ns) fct(ns) is_cross_dc
```

每行表示一条完成的 query flow：

```text
src_node dst_node pg dport flow_size_bytes start_time_ns fct_ns is_cross_dc
```

Python 会额外打印：

- query flow 总数。
- cross-DC flow 数量。
- intra-DC flow 数量。
- 平均 FCT。
- P50/P95/P99 FCT。
- cross-DC 与 intra-DC FCT 对比。

### PFC 文件

```text
results/workload/pfc/pfc_frp_load20pct.txt
```

记录 PFC 相关事件或统计。用于观察拥塞控制是否触发大量暂停帧，以及不同算法下 PFC 行为差异。

### 结果图

```text
results/workload/workload_frp_cc13_load20pct.png
```

图中包含两部分：

1. Receiver throughput：
   - 展示接收端 flow throughput。
   - 最多绘制前 15 条 flow。

2. Queue length：
   - Python 自动从日志中找队列监控数据。
   - 按平均队列长度排序，绘制 top congestion points。
   - 用于快速观察瓶颈交换机端口的队列变化。

## 5. 文件命名规则

Python 使用：

```text
{algo_name_lower}_load{load_percent}
```

例如：

| LOAD | 后缀 |
| --- | --- |
| `0.2` | `load20pct` |
| `0.05` | `load5pct` |
| `0.125` | `load12p5pct` |

完整例子：

```text
fct_frp_load20pct.txt
pfc_frp_load20pct.txt
query-flow-frp_load20pct.txt
workload_frp_cc13_load20pct.png
algo_cc13_load20pct.log
```

这种命名避免了 `0.2` 中的小数点和文件扩展名混淆。

## 6. 比较不同算法的推荐流程

保持 `config-workload.txt` 不变，只改变算法运行命令：

```bash
python3 run_single_workload_algo.py 1 DCQCN
python3 run_single_workload_algo.py 3 HPCC
python3 run_single_workload_algo.py 7 TIMELY
python3 run_single_workload_algo.py 8 DCTCP
python3 run_single_workload_algo.py 13 FRP
python3 run_single_workload_algo.py 14 ROCC
```

这样每个算法使用相同的：

- topology。
- CDF。
- LOAD。
- START_TIME。
- SIMULATOR_STOP_TIME。
- FLOW_LAUNCH_END_TIME。
- query flow。
- random seed，除非你手动修改。

比较指标建议：

| 指标 | 文件/来源 | 用途 |
| --- | --- | --- |
| 平均 FCT | Python 终端输出 / FCT 文件 | 看整体完成时间 |
| P99 FCT | Python 终端输出 / FCT 文件 | 看尾延迟 |
| Query Flow FCT | query flow FCT 文件 | 看关键请求流性能 |
| Cross-DC Query FCT | Python query flow 分析 | 看跨 DC 请求流表现 |
| Queue Length | 结果图 / 日志 | 看拥塞和排队 |
| Throughput | 结果图 / 日志 | 看吞吐稳定性 |
| PFC | PFC 文件 | 看是否频繁触发 pause |

## 7. 修改实验参数的位置

如果你想改变实验负载或时间，不改 Python，直接改：

```text
simulator/ns-3.39/examples/PowerTCP/config-workload.txt
```

常改字段：

```text
LOAD 0.2
START_TIME 0.005
SIMULATOR_STOP_TIME 0.020
FLOW_LAUNCH_END_TIME 0.01
CDF_FILE_NAME examples/PowerTCP/Alistorage.txt
QUERY_FLOW_FILE examples/PowerTCP/query-flow.txt
```

如果你想改变输出文件名规则，改：

```text
FCT_OUTPUT_TEMPLATE results/workload/fct/fct_{suffix}.txt
PFC_OUTPUT_TEMPLATE results/workload/pfc/pfc_{suffix}.txt
QUERY_FCT_OUTPUT_TEMPLATE results/workload/fct/query-flow-{suffix}.txt
```

模板中可用变量：

| 变量 | 含义 |
| --- | --- |
| `{suffix}` | 算法和负载组成的后缀，例如 `frp_load20pct` |
| `{algo}` | 小写算法名，例如 `frp` |
| `{ccMode}` | 算法编号，例如 `13` |
| `{load}` | config 中原始 LOAD 值，例如 `0.2` |
| `{seed}` | random seed |

## 8. 当前需要注意的点

1. `CC_MODE` 在 config 中仍存在，但单次运行时 Python 传入的 `--algorithm` 会覆盖它。

2. `SIMULATOR_STOP_TIME` 控制仿真停止时间，C++ 解析后直接赋给 `simulator_stop_time`，并在 `Simulator::Stop(Seconds(simulator_stop_time))` 处生效。早期版本中存在 `simulator_stop_time = END_TIME;` 的覆盖逻辑，现已注释移除，`END_TIME` 字段不再使用。

3. `BACKGROUND_FLOW_FILE` 当前不是实际背景流输入文件。背景流由 CDF 和 `LOAD` 生成。

4. Python 每次运行都会 build 一次 C++，这样比较稳妥，但多算法批量实验会比较慢。

5. 如果结果图没有队列曲线，优先检查对应算法是否打印了 Python 能识别的日志：

```text
[DCQCN_QLEN]
[FRP_DATA_SW]
```

6. 如果 query flow FCT 文件为空，检查：

```text
QUERY_FLOW_FILE
SIMULATOR_STOP_TIME
query flow start_time
flow size
```

确保 query flow 在仿真结束前能完成。

