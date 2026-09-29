# crossDC 示例编译与运行说明

本文说明两个跨数据中心 RDMA 示例的编译、运行入口、依赖配置文件和常用脚本。所有 ns-3 命令默认从仓库根目录进入 `simulator/ns-3.39` 后执行。

> 程序内部结构见 [PROGRAM_STRUCTURE.md](PROGRAM_STRUCTURE.md)，workload 流量设计见 [WORKLOAD_FLOW_ANALYSIS.md](WORKLOAD_FLOW_ANALYSIS.md)，workload 运行细节见 `simulator/ns-3.39/examples/PowerTCP/WORKLOAD_EXPERIMENT_GUIDE.md`。

两个示例对应两条实验轨道：

| 轨道 | 源文件 | 配置 | 流量来源 | 用途 |
|------|--------|------|---------|------|
| **Track 1：burst** | `crossDC-evaluation.cc` | `config.txt` | `flow.txt` 确定性流 | 固定突发场景，复现论文基线 |
| **Track 2：workload** | `crossDC-evaluation-workload.cc` | `config-workload.txt` | CDF 镜像背景流 + `query-flow.txt` 查询流 | 真实 workload 下的算法对比 |

---

## 1. Track 1：flow.txt 驱动版本 crossDC-evaluation

### 1.1 源文件与构建目标

源文件：`simulator/ns-3.39/examples/PowerTCP/crossDC-evaluation.cc`
CMake 目标名：`crossDC-evaluation`

```bash
cd simulator/ns-3.39
./ns3 build crossDC-evaluation
```

若刚修改 `examples/PowerTCP/CMakeLists.txt` 或新增目标，先重新配置：

```bash
cd simulator/ns-3.39
./configure.sh
./ns3 build crossDC-evaluation
```

构建产物：`simulator/ns-3.39/build/examples/PowerTCP/ns3.39-crossDC-evaluation-optimized`

### 1.2 依赖配置文件

默认主配置：`simulator/ns-3.39/examples/PowerTCP/config.txt`

| 文件 | 作用 |
| --- | --- |
| `examples/PowerTCP/config.txt` | 主配置：拓扑、流文件、输出文件、CC 参数、缓存、监控参数 |
| `examples/PowerTCP/topology.txt` | 真实 crossDC 拓扑（节点数、交换机列表、链路带宽/时延） |
| `examples/PowerTCP/flow.txt` | 流输入：首行流数量，后续每行 `src dst pg dport size start_time` |
| `FCT_OUTPUT_FILE` 指向的文件 | RDMA QP 完成时输出 FCT 记录 |
| `PFC_OUTPUT_FILE` 指向的文件 | PFC 事件输出 |

`crossDC-evaluation --algorithm=N` 会覆盖 `config.txt` 中的 `CC_MODE`。

### 1.3 直接运行命令

```bash
cd simulator/ns-3.39
./ns3 run "crossDC-evaluation --conf=examples/PowerTCP/config.txt --algorithm=13"
```

或直接运行优化版二进制：

```bash
cd simulator/ns-3.39
./build/examples/PowerTCP/ns3.39-crossDC-evaluation-optimized \
  --conf=examples/PowerTCP/config.txt \
  --algorithm=13
```

常用算法编号：

| ccMode | 算法 |
| --- | --- |
| `1` | DCQCN |
| `3` | HPCC |
| `7` | TIMELY |
| `12` | Bifrost |
| `13` | FRP |
| `14` | ROCC |

### 1.4 相关脚本

仓库根目录：

```bash
python3 run_single_algo.py 13 FRP      # 单算法
./run_all_algos.sh                     # 批量 5 算法
```

`run_single_algo.py` 行为：

- 改写 `config.txt`（`SIMULATOR_STOP_TIME`、`CC_MODE`、`QLEN_MON_*`）。
- 编译并运行 `crossDC-evaluation`。
- 日志写入 `/tmp/algo_cc<ccMode>.log`。
- FCT/PFC 写入 `results/fct/`、`results/pfc/`。
- 固定监控 **Switch 93 Port 1** 队列，图像输出 `results/burst_<algo>_cc<ccMode>.png`。

`run_all_algos.sh` 依次调用 DCQCN/HPCC/TIMELY/FRP/ROCC。

---

## 2. Track 2：CDF workload 版本 crossDC-evaluation-workload

### 2.1 源文件与构建目标

源文件：`simulator/ns-3.39/examples/PowerTCP/crossDC-evaluation-workload.cc`
CMake 目标名：`crossDC-evaluation-workload`

```bash
cd simulator/ns-3.39
./ns3 build crossDC-evaluation-workload
```

构建产物：`simulator/ns-3.39/build/examples/PowerTCP/ns3.39-crossDC-evaluation-workload-optimized`

### 2.2 依赖配置文件

默认主配置：`simulator/ns-3.39/examples/PowerTCP/config-workload.txt`

| 文件 | 作用 |
| --- | --- |
| `examples/PowerTCP/config-workload.txt` | workload 主配置：拓扑、CDF、负载、起止时间、查询流、输出模板、CC/缓存/监控参数 |
| `examples/PowerTCP/topology.txt` | 真实拓扑，程序自动发现主机与 host-facing leaf |
| `examples/PowerTCP/Alistorage.txt` | CDF 流大小分布（背景流来源），由 `CDF_FILE_NAME` 指定 |
| `examples/PowerTCP/query-flow.txt` | 查询流文件（被测前景流），由 `QUERY_FLOW_FILE` 指定 |
| `examples/PowerTCP/cdf.c` / `cdf.h` | CDF 读取与随机流大小生成 |
| `FCT_OUTPUT_TEMPLATE` / `PFC_OUTPUT_TEMPLATE` / `QUERY_FCT_OUTPUT_TEMPLATE` | 输出文件模板，含 `{suffix}` 占位符 |

> 注意：`FLOW_FILE`、`BACKGROUND_FLOW_FILE`、`ENABLE_FLOW_FILE_BACKGROUND` 字段会被解析打印，但当前实现**并不**读取 `flow.txt` / `BACKGROUND_FLOW_FILE` 作为确定性背景流。`ENABLE_FLOW_FILE_BACKGROUND` 实际仅作为"是否生成 CDF 镜像背景流"的开关。workload 参数全部从配置文件读取，无对应命令行参数。

`config-workload.txt` 关键字段：

```text
TOPOLOGY_FILE examples/PowerTCP/topology.txt
CDF_FILE_NAME examples/PowerTCP/Alistorage.txt
QUERY_FLOW_FILE examples/PowerTCP/query-flow.txt
LOAD 0.2
START_TIME 0.005
FLOW_LAUNCH_END_TIME 0.01
SIMULATOR_STOP_TIME 0.080
ENABLE_FLOW_FILE_BACKGROUND 1
FCT_OUTPUT_TEMPLATE results/workload/fct/fct_{suffix}.txt
PFC_OUTPUT_TEMPLATE results/workload/pfc/pfc_{suffix}.txt
QUERY_FCT_OUTPUT_TEMPLATE results/workload/fct/query-flow-{suffix}.txt
CC_MODE 13              # 被 --algorithm 覆盖
NIC_DELAY 15000
KMAX_MAP / KMIN_MAP / PMAX_MAP ...
BUFFER_SIZE 50
```

### 2.3 命令行参数

`crossDC-evaluation-workload` 接受的命令行参数（其余 workload 参数均来自配置文件）：

| 参数 | 含义 |
| --- | --- |
| `--conf` | 主配置文件，默认 `examples/PowerTCP/config-workload.txt` |
| `--algorithm` | 拥塞控制算法编号，覆盖配置中的 `CC_MODE` |
| `--randomSeed` | 随机种子；0 表示按当前时间初始化 |
| `--fctOutputFile` | 全量 FCT 输出路径（覆盖模板） |
| `--pfcOutputFile` | PFC 输出路径（覆盖模板） |
| `--queryFlowFctFile` | 查询流 FCT 输出路径（覆盖模板） |
| `--wien` / `--delayWien` | PowerTCP / Theta-PowerTCP 开关 |
| `--SERVER_COUNT` / `--LEAF_COUNT` | 每个自动发现 leaf 最多使用主机数 / 最多使用 leaf 数；0=全部 |
| `--SPINE_COUNT` / `--LEAF_SERVER_CAPACITY` / `--SPINE_LEAF_CAPACITY` | 容量推导兜底参数 |
| `--windowCheck` | 窗口检查开关 |

> 改负载、起止时间、CDF、查询流等内容，**改 `config-workload.txt`，不要试图用命令行传 `--load`/`--END_TIME`/`--queryRequestRate` 等**——这些参数已不在命令行注册。

### 2.4 直接运行命令

```bash
cd simulator/ns-3.39
./ns3 run "crossDC-evaluation-workload --conf=examples/PowerTCP/config-workload.txt --algorithm=13 --randomSeed=7"
```

或直接运行优化版二进制：

```bash
cd simulator/ns-3.39
./build/examples/PowerTCP/ns3.39-crossDC-evaluation-workload-optimized \
  --conf=examples/PowerTCP/config-workload.txt \
  --algorithm=13 \
  --randomSeed=7 \
  --fctOutputFile=results/workload/fct/fct_frp_load20pct.txt \
  --pfcOutputFile=results/workload/pfc/pfc_frp_load20pct.txt \
  --queryFlowFctFile=results/workload/fct/query-flow-frp_load20pct.txt
```

### 2.5 相关脚本

仓库根目录：

```bash
python3 run_single_workload_algo.py 13 FRP      # 单算法
./run_all_algos_workload.sh                     # 批量 5 算法
```

`run_single_workload_algo.py` 行为：

- 只读 `config-workload.txt`，提取 `LOAD` 和输出模板。
- 按算法名+负载生成唯一后缀（如 `frp_load20pct`）。
- 编译并运行 `crossDC-evaluation-workload`，仅传 `--conf`/`--algorithm`/`--randomSeed`/`--fctOutputFile`/`--pfcOutputFile`/`--queryFlowFctFile`。
- 日志写入 `dump/workload/algo_cc<ccMode>_load<load_tag>.log`。
- 自动检测 Top 5 拥塞端口绘图，输出 `results/workload/workload_<algo>_cc<ccMode>_load<load_tag>.png`。
- 解析查询流 FCT 并打印 cross-DC / intra-DC 对比。

`run_all_algos_workload.sh` 依次调用 DCQCN/HPCC/TIMELY/FRP/ROCC，可透传参数给 `run_single_workload_algo.py`。

### 2.6 修改实验参数的位置

改 workload 实验，改 `config-workload.txt`：

```text
LOAD 0.2                      # 背景流负载
START_TIME 0.005              # 背景流开始时间
FLOW_LAUNCH_END_TIME 0.01     # 背景流发起截止时间
SIMULATOR_STOP_TIME 0.080     # 仿真停止时间
CDF_FILE_NAME examples/PowerTCP/Alistorage.txt
QUERY_FLOW_FILE examples/PowerTCP/query-flow.txt
```

模板变量：`{suffix}`=`{algo}_load{load_tag}`、`{algo}`、`{ccMode}`、`{load}`、`{seed}`。

---

## 3. 输出文件命名规则（Track 2）

以 `python3 run_single_workload_algo.py 13 FRP`、`LOAD=0.2` 为例，后缀为 `frp_load20pct`：

| 类型 | 路径 |
|------|------|
| 日志 | `dump/workload/algo_cc13_load20pct.log` |
| 全量 FCT | `results/workload/fct/fct_frp_load20pct.txt` |
| PFC | `results/workload/pfc/pfc_frp_load20pct.txt` |
| 查询流 FCT | `results/workload/fct/query-flow-frp_load20pct.txt` |
| 结果图 | `results/workload/workload_frp_cc13_load20pct.png` |

负载小数点转为 `p`，如 `0.125` → `load12p5pct`。

---

## 4. 常见问题排查

- **结果图无队列曲线**：检查对应算法是否打印了 Python 能识别的日志标签——DCQCN/HPCC/TIMELY 用 `[DCQCN_QLEN]`，FRP/ROCC 用 `[FRP_DATA_SW]`。DCQCN 路径还受 `switch-node.cc` 中 `m_id == 32 || m_id == 85 || m_id == 93` 硬编码限制。
- **查询流 FCT 文件为空**：检查 `QUERY_FLOW_FILE`、`SIMULATOR_STOP_TIME`、查询流 `start_time` 与流大小，确保查询流在仿真结束前能完成。
- **HPCC/TIMELY 完成率低**：`SIMULATOR_STOP_TIME` 过短会导致长 RTT 算法的查询流未完成，建议提高到 0.2s 以上（查询流有"全部完成后 1ms 提前停止"机制，不会拖慢快的算法）。
- **移植到新环境**：检查脚本中的绝对路径 `NS3_DIR`、`RESULTS_DIR`、`DUMP_DIR` 以及 C++ 中 query FCT 默认路径。
