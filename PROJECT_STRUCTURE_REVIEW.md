# 当前项目结构审查与重构建议

审查基线：`e53f00c`（`增加LonghaulCC 示例目录`）  
审查日期：2026-09-22  
审查方式：只读审查；本次未修改源代码、配置文件或 CMake 构建逻辑。

## 1. 审查范围和结论边界

本次审查按“项目维护边界”理解整个工程，覆盖了：

- 仓库根目录、`run_scripts/`、`plot scripts/`、`results/`、`dump/` 和 `docs/` 的组织方式；
- `simulator/ns-3.39/examples/` 下的 `LonghaulCC`、`PowerTCP` 示例、配置、拓扑、流量和 Python 运行/分析脚本；
- `simulator/ns-3.39/src/point-to-point/` 中与本项目直接相关的 Qbb、RDMA、交换机、MMU、FRP 和网关模型；
- CMake 注册、ns-3 目标映射、实验程序入口、输入读取、输出文件和分析脚本之间的调用关系；
- 当前提交后的构建目标列表和关键输入文件的静态有效性。

`simulator/bake/`、`simulator/netanim-3.109/` 以及 ns-3 的通用上游模块被视为第三方/基础依赖，没有对这些目录逐行重审。它们只有在影响本项目入口、构建或运行链路时才被纳入分析。

本文件是结构审查和重构建议，不是代码修改计划的批准，也不代表已经实施了下述重构。

## 2. 总体结论

当前工程是一个能够承载多组网络仿真实验的研究型原型，但“实验编排、场景代码、网络模型、统计输出”四类职责已经交叉。当前最主要的维护风险不是代码风格，而是以下四个问题同时存在：

1. **同一个实验参数有多个事实来源。** 配置文件、命令行参数、Python runner 中的常量、C++ 默认值和分析脚本中的场景表都可能决定同一项行为。
2. **多个入口重复实现同一套基础流程。** `crossDC-evaluation.cc`、`crossDC-evaluation-workload.cc` 和 `longhaul-convergence.cc` 都分别实现了拓扑读取、地址分配、路由、RDMA 安装、流调度和输出逻辑。
3. **输入拓扑并不完全是声明式的。** 文件中有拓扑，但程序仍依赖固定节点编号、数据中心范围、设备索引和单个 DCI 对端。
4. **输出格式和分析逻辑没有统一的测量契约。** 一部分结果依赖空格分隔文件，一部分结果使用 CSV，一部分分析依赖 stdout 文本、端口号或固定节点编号。

最近的 `PowerTCP` → `LonghaulCC` 迁移改善了目录归属和 CMake target 的重复注册问题，这是正确方向；但它主要解决了“文件放在哪里”和“target 是否重复”的问题，还没有解决公共实验基础设施重复、配置事实源分散和模型层耦合问题。

推荐采用**渐进式重构**：先固定实验契约和可复现性，再抽取少量公共基础设施，最后再处理 `RdmaHw`、`SwitchNode` 等模型内部的策略解耦。不建议一次性重写拓扑、入口、拥塞控制算法和 ns-3 模型。

## 3. 当前目录和职责分布

### 3.1 仓库级目录

| 目录 | 当前职责 | 主要问题 |
|---|---|---|
| `simulator/ns-3.39/` | ns-3 构建树、示例和自定义模型 | 项目实验代码与基础模拟器代码混在一个大目录内，入口和模型边界不够明显 |
| `simulator/ns-3.39/examples/LonghaulCC/` | 新迁移的长距离跨 DC 实验 | 目录已经清晰，但入口仍包含大量通用网络搭建逻辑 |
| `simulator/ns-3.39/examples/PowerTCP/` | legacy burst、workload、fairness 等实验 | 名称已经不能完全代表内容，其中包含通用 cross-DC 实验和重复 CDF 实现 |
| `run_scripts/` | 运行、批量实验、解析和绘图 | 多个脚本互相独立，路径和输出约定不一致；部分脚本会修改原始配置 |
| `plot scripts/` | 结果绘图 | 目录名含空格，脚本常依赖硬编码文件名、列号和绝对路径 |
| `docs/` | 研究记录、运行说明、阶段报告 | 根 `.gitignore` 忽略整个 `docs/`；部分文档仍指向迁移前的 `PowerTCP/longhaul-*` 路径 |
| `results/`、`dump/` | 运行结果、日志和中间数据 | 运行结果没有统一的实验清单和 schema；目录结构由不同脚本分别决定 |

当前根目录只有一行简短 README，不能承担项目入口、实验分类、构建和结果组织说明的职责。

### 3.2 当前可构建的实验入口

`./ns3 show targets` 当前确认到以下项目实验 target：

| target | 源入口 | 当前定位 |
|---|---|---|
| `longhaul-convergence` | `examples/LonghaulCC/longhaul-convergence.cc` | 新的长距离 DCI 基线和研究模式 |
| `crossDC-evaluation` | `examples/PowerTCP/crossDC-evaluation.cc` | 传统 burst/跨 DC 评估 |
| `crossDC-evaluation-workload` | `examples/PowerTCP/crossDC-evaluation-workload.cc` | 背景 workload + query flow |
| `powertcp-evaluation-burst` | `examples/PowerTCP/powertcp-evaluation-burst.cc` | legacy burst 实验 |
| `powertcp-evaluation-fairness` | `examples/PowerTCP/powertcp-evaluation-fairness.cc` | fairness 实验 |
| `powertcp-evaluation-workload` | `examples/PowerTCP/powertcp-evaluation-workload.cc` | legacy workload 实验 |

`examples/PowerTCP/` 下还有部分源文件不一定被当前 CMake target 使用。后续需要区分“源码存在”和“入口可运行”，不能仅根据文件名判断实验是否有效。

### 3.3 LonghaulCC 当前入口内部结构

`longhaul-convergence.cc` 目前大致包含以下职责：

| 代码区域 | 职责 |
|---|---|
| 全局变量区 | 配置值、路径、拥塞控制参数、节点容器、路由表、流表、输出流和 DCI 状态 |
| `LoadFlows`、`StartFlow` | 流量文件解析和 RDMA client 安装 |
| 地址/路由函数 | 地址分配、BFS 路由、静态 RDMA 路由表 |
| 拓扑创建代码 | 节点、Qbb 链路、PFC/ECN trace 和 DCI 设备识别 |
| RDMA/MMU 配置 | NIC、`RdmaHw`、交换机 MMU 和拥塞控制属性 |
| 采样和完成回调 | FCT、发送速率、接收 goodput、DCI 链路、PFC 统计 |
| `ParseConfig` | 约几十项 key-value 配置的手工解析 |
| `main` | 命令行优先级、所有初始化、调度、运行、关闭和输出 |

入口已经超过单个实验场景应承担的复杂度。尤其是全局状态使得每一个辅助函数都隐式依赖整个程序的运行上下文。

此外，`longhaul-research.h` 不是普通的声明头文件，而是通过 `#include` 直接把研究控制的实现嵌入 `longhaul-convergence.cc`。它直接访问 `serverAddress`、`flows`、`nbr2if`、`dci_left_device`、`n` 等入口文件内部的全局对象，属于隐藏耦合。

## 4. 当前调用链和数据流

### 4.1 构建链

```text
examples/CMakeLists.txt
        │ 自动发现带 CMakeLists.txt 的示例目录
        ├── examples/LonghaulCC/CMakeLists.txt
        │       └── longhaul-convergence + cdf.c
        └── examples/PowerTCP/CMakeLists.txt
                └── crossDC / workload / PowerTCP legacy targets

各 target
        │ 链接
        └── libpoint-to-point
                ├── QbbNetDevice / QbbChannel
                ├── RdmaDriver / RdmaHw / RdmaQueuePair
                ├── SwitchNode / SwitchMmu
                ├── FrpRateCalculator
                └── atcGateway 等扩展模型
```

`LonghaulCC` 作为 sibling example 目录被自动发现，当前 CMake target 已经可以区分；不需要再把 Longhaul target 放回 `PowerTCP`。

### 4.2 运行链

```text
Python runner 或手工命令
        │ 选择配置、算法、场景、seed/run、输出目录
        ▼
ns-3 示例可执行文件
        │
        ├── 读取 key-value 配置
        ├── 读取 topology 文件
        ├── 读取 flow/query/CDF 输入
        ├── 创建 Node、Qbb 链路、IP 地址和静态路由
        ├── 安装 SwitchNode、MMU、RdmaHw、RdmaDriver
        ├── 安装 background/query/RDMA flows
        ├── 通过 trace 和定时事件采样
        └── 写出 FCT、PFC、rate、goodput、link、metadata、stdout
                ▼
        analyze / plot 脚本
                └── 汇总 CSV、生成图、输出研究结论
```

这个链条目前没有明确的中间对象表示“有效实验配置”。因此 runner 认为的参数、C++ 实际使用的参数和 analyzer 假设的参数可能不是同一份内容。

### 4.3 模型层调用关系

```text
RdmaClientHelper
        ▼
RdmaDriver
        ▼
RdmaHw ───────── RdmaQueuePair / RdmaRxQueuePair
        │                  │
        ▼                  └── 发送/接收计数、CC 状态、完成回调
QbbNetDevice ── QbbChannel
        │
        ├── PFC / ECN / queue / trace
        ▼
SwitchNode ── SwitchMmu
        ├── DCQCN / HPCC / PINT / FRP / Bifrost / gateway feedback
        └── FrpRateCalculator / atcGateway
```

当前 `RdmaHw`、`RdmaQueuePair`、`SwitchNode` 同时包含通用传输逻辑和多种算法的状态、参数及分支。示例程序又直接访问 `RdmaHw`、`RdmaDriver` 和 QP 内部成员，导致模型的内部数据布局实际上成为实验入口 API。

## 5. 输入、配置和输出审查

### 5.1 LonghaulCC

主要输入：

- `examples/LonghaulCC/config-longhaul.txt`：公共参数、拓扑/流量路径、输出路径、CC 参数、采样间隔、seed/run 和 DCI 节点；
- `topology-longhaul.txt`：节点与链路拓扑；
- `flow-longhaul-s0.txt` 到 `flow-longhaul-s5.txt`：场景流量；
- 命令行：`--cc`、`--algorithm`、`--conf`、seed/run、研究控制参数等；
- `run-longhaul-baseline.py`：场景 stop time、算法映射、结果目录和每次运行的配置快照。

主要输出：

- `fct.csv`、`pfc.csv`；
- `sender-rate.csv`、`receiver-goodput.csv`、`dci-link.csv`；
- `metadata.json`、runner 的 `runner-metadata.json`（研究 runner 使用 `run.json`）/日志以及配置快照；
- `run-research.py` 额外生成的研究控制、输入快照和证据文件。

当前优点是 Longhaul runner 已经按 scenario/algorithm/seed/run 分目录，并保存了一部分输入和 metadata。主要问题是：

- `run-longhaul-baseline.py` 和 `analyze-longhaul.py` 分别维护一套 S0--S5 信息，场景定义存在漂移风险；
- analyzer 中还根据端口号推断 flow role，结果语义依赖实现细节；
- C++、runner 和 analyzer 对 DCI、拓扑节点编号、输出列含义各自有假设；
- C++ 通过两次命令行解析实现覆盖关系，配置优先级不够显式；
- `longhaul-research.h` 复用 Longhaul 全局状态，研究模式不能独立测试。

### 5.2 PowerTCP burst/crossDC

主要输入是 `examples/PowerTCP/config.txt`、`config-burst.txt`、拓扑文件和 burst flow 文件。当前入口同时支持配置文件和命令行算法选择，外层 Python 脚本还会直接改写配置文本。

主要输出仍然是 legacy 格式：空格分隔的 FCT/PFC 文件、stdout 中的 throughput/queue 文本和脚本自行解析的图片数据。它与 Longhaul 的 CSV 输出不是同一套契约。

当前 `config-burst.txt` 在 `QLEN_MON_END_TIME` 前存在一个控制字符行（显示为 `\001 2000000000`），说明配置文件目前缺少格式校验和提交前检查。

### 5.3 PowerTCP workload

`crossDC-evaluation-workload.cc` 同时负责：

- workload CDF 读取和背景流生成；
- query flow 文件读取；
- 拓扑自动识别、IP 分配、路由和 RDMA 安装；
- query FCT、背景流、吞吐和队列采样；
- gateway、Bifrost、ATC、PowerTCP 等多个模式的配置。

这里存在几个明显的契约问题：

- C++ 通过 `while (!conf.eof())` 读取配置；
- `FCT_OUTPUT_TEMPLATE`、`PFC_OUTPUT_TEMPLATE`、`QUERY_FCT_OUTPUT_TEMPLATE` 会被读取后放入 `ignored`，并非真正由 C++ 使用；
- `BACKGROUND_FLOW_FILE` 等配置项与实际“按 CDF 生成背景流”的路径并存，配置含义不清；
- workload 仍然使用固定的 DC 节点范围，例如 `0..275` 和 `277..532`；
- `PrintResults` 中存在 `j == 16` 这种只对某个旧场景成立的接收端硬编码；
- “自动识别拓扑”与后续依赖固定节点范围的 workload 分类并不一致。

### 5.4 结果组织

当前结果目录来自多个脚本约定：`results/longhaul/`、`results/research-*`、`results/unified-cc/`、`results/fct/`、`results/workload/` 等。不同入口的结果不能仅通过目录名判断：

- 输入 topology/flow 可能没有随结果完整保存；
- 参数快照的字段不统一；
- 成功通常只用进程退出码判断，未统一记录预期流数、完成流数、接收字节数和 stop reason；
- 部分分析依赖 stdout 文本而非结构化结果；
- 旧结果和新结果的 schema 没有版本号。

## 6. 主要结构问题和优先级

### P0：先解决正确性、可复现性和契约问题

#### P0-1：配置存在多个事实源

同一项行为可能由以下位置共同决定：

- C++ 全局默认值；
- `config*.txt`；
- `--algorithm`、`--cc`、`--seed` 等命令行；
- Python runner 中的 `SCENARIOS`、`ALGORITHMS`、stop time 和 quick 参数；
- analyzer 中的 `STAGES`、端口和 flow role 推断。

例如 Longhaul 同时有 `CC_MODE`、`--algorithm` 和 `--cc`；workload 的 `config-quick.txt` 又作为脚本层覆盖项存在。当前虽然部分地方实现了“命令行覆盖配置”，但覆盖顺序没有形成项目级规范。

**影响：** 改一个配置不一定改变实际实验；复现实验需要同时阅读 C++、runner 和 analyzer。

#### P0-2：拓扑输入不是完整的声明式接口

程序仍依赖：

- 固定 node ID 表示 host、leaf、spine、gateway；
- 固定 host 范围表示 DC；
- 固定 device index 表示主机 NIC；
- 固定 DCI 两端和单个 DCI 设备；
- 固定端口号、query receiver 或 flow role。

**影响：** 改节点数量、重排拓扑、增加 DCI 或复用入口时，容易得到“程序运行了但实验语义变了”的结果。

#### P0-3：输出不是统一的测量契约

Longhaul 的结构化 CSV、PowerTCP 的 legacy 文本和 stdout 解析并存。FCT、goodput、DCI link rate、PFC 的字段定义没有由公共 schema 约束，分析脚本还会根据端口号、节点号和文件名推断语义。

**影响：** 不同算法或入口的结果难以直接比较；分析脚本对实现细节敏感；缺少列时可能静默生成错误图。

#### P0-4：当前模型存在实验有效性边界

`RdmaHw::GetNxtPacket()` 中把 `GetBytesLeft()` 赋给 `uint32_t payload_size`，并且发送字节计数也使用 `uint32_t`。因此当前 checkout 不能把 1 TB 流当作有效实验输入；Longhaul 目录中对这一限制已有说明，但它还应该变成程序启动时的显式校验，而不是依赖文档提醒。

同样，`PowerTCP/cdf.c` 的 `load_cdf()` 在 `fopen()` 失败后只调用 `perror` 仍继续 `fgets`，`gen_random_cdf()` 在检查 `table` 之前就访问 `table->min_cdf`。这些是输入失败路径的稳定性问题，不属于单纯风格问题。

### P1：解决维护成本和重复实现

#### P1-1：三个大型 C++ 入口复制基础流程

`crossDC-evaluation.cc`、`crossDC-evaluation-workload.cc` 和 `longhaul-convergence.cc` 都包含配置解析、拓扑读取、地址分配、路由、RDMA 初始化、流安装和结果输出。后续修复一个路由、地址或统计问题时，必须判断哪些入口需要同步修改。

#### P1-2：全局可变状态和单体 `main`

Longhaul 在全局层保存配置、节点、流、路由、输出流和 DCI 状态，`main` 继续完成几乎所有初始化步骤。workload 入口的全局状态更多，并混入 gateway、背景 workload、query flow 和多种控制模式。

这导致：

- 辅助函数难以单独测试；
- 同一进程内难以安全运行多个实验实例；
- 研究控制只能通过包含入口实现的 header 访问内部状态；
- 对象生命周期和关闭顺序依赖全局变量。

#### P1-3：实验层直接依赖模型内部布局

示例代码直接访问 `RdmaDriver::m_rdma`、QP 的发送/接收成员、Qbb 设备队列和 trace 细节。研究代码也直接使用全局路由表和设备对象。

这样做短期便于实验，但模型重命名、封装字段或替换统计实现时，所有入口都会被牵连。建议逐步提供明确的观察接口，而不是马上引入复杂的插件系统。

#### P1-4：研究控制不是独立模块

`longhaul-research.h` 直接包含实现并依赖 Longhaul 的全部全局上下文。它同时负责 trace 过滤、虚拟队列、预测、CNP 生成和采样输出。

建议先把它变为 `.h/.cc` 和一个显式上下文对象；上下文只提供研究控制需要的流表、拓扑查询、发送控制包和输出接口。

### P2：解决目录、命名和长期清理问题

- `PowerTCP` 目录包含非 PowerTCP 名称的通用 cross-DC 入口；
- CDF 实现至少在 Longhaul 和 PowerTCP 各有一份；
- `run_scripts/`、`plot scripts/`、示例目录内部都有运行脚本，入口位置不统一；
- `plot scripts` 含空格，影响命令行使用和自动化；
- 多处脚本和旧 C++ 文件含机器相关绝对路径；
- 根 `.gitignore` 忽略 `docs/`，导致已经存在的结构文档不容易随代码版本化；
- 部分文档仍引用迁移前的 `examples/PowerTCP/longhaul-*` 路径；
- 没有对配置解析、输出 schema 和最小 smoke run 的自动检查。

这些问题会增加维护成本，但应排在 P0 的实验契约和 P1 的公共代码抽取之后处理。

## 7. 推荐的目标结构

推荐目标不是立即移动全部文件，而是逐步形成以下依赖方向：

```text
实验 manifest / runner / analysis
              │
              ▼
场景层：LonghaulCC、PowerTCP workload、legacy scenario
              │
              ▼
公共实验基础设施：配置、拓扑、flow、路由、输出契约
              │
              ▼
ns-3 模型层：Qbb、RDMA、SwitchNode、MMU、控制器
```

依赖方向应满足：

- runner 不被 C++ 输出字符串反向驱动；
- analyzer 只消费 manifest 和结构化结果，不读取模型内部变量；
- 场景层不直接修改模型对象的非公开状态；
- 模型层不依赖 `results/`、runner 或 docs；
- 一个实验运行只有一个有效配置对象和一个输出根目录。

在保留当前 ns-3 目录布局的前提下，长期可以收敛为：

```text
PROJECT_ROOT/
├── experiments/
│   ├── manifests/          # 实验矩阵和场景参数
│   ├── runners/            # 统一运行入口，旧脚本可作为兼容包装
│   ├── analysis/           # 统一分析和绘图
│   └── schemas/            # manifest 与 CSV 字段定义
├── results/<experiment>/<run-id>/
├── simulator/ns-3.39/examples/
│   ├── common/             # 只放少量不依赖具体算法的实验辅助代码
│   ├── LonghaulCC/
│   └── PowerTCP/
└── simulator/ns-3.39/src/point-to-point/
    └── 模型层
```

但这个目录目标不应作为第一步。第一阶段可以继续使用 `run_scripts/` 和现有 example 路径，只统一它们的输入、输出和公共函数；迁移目录应在兼容性稳定后进行。

## 8. 推荐的重构方案

### 8.1 第一步：建立单一有效配置和运行清单

先定义一个实验级 manifest。格式可以先沿用简单 key-value，不必为了结构化而立即引入 YAML 依赖。至少包含：

```text
experiment_id
scenario
algorithm
cc_mode
topology_file
flow_file
payload_bytes
stop_time_s
rng_seed
rng_run
dci_left
dci_right
output_root
```

算法私有参数放在命名空间下，例如 `dcqcn.*`、`timely.*`、`research.*`；所有时间、速率和字节字段使用带单位的名称，例如 `_s`、`_ns`、`_us`、`_bps`、`_bytes`。

明确唯一优先级：

```text
程序默认值 < 场景 manifest < 显式命令行参数
```

程序只解析一次参数，并生成一份 `config.effective.txt` 或 `manifest.json`。runner、C++ 程序和 analyzer 都读取或引用这份有效配置，不再分别维护相同的场景表。

### 8.2 第二步：统一每次运行的输出目录

建议所有入口逐步采用以下结构：

```text
results/<experiment>/<scenario>/<algorithm>/seed<seed>-run<run>/
├── manifest.json
├── config.effective.txt
├── inputs/
│   ├── topology.txt
│   └── flows.txt
├── stdout.log
├── status.json
└── metrics/
    ├── fct.csv
    ├── pfc.csv
    ├── sender-rate.csv
    ├── receiver-goodput.csv
    └── dci-link.csv
```

`status.json` 至少记录：

- command、git commit、开始/结束时间、退出码；
- expected flow count、completed flow count；
- 发送和接收总字节；
- 实际 stop reason；
- 输入文件 hash 和输出 schema version。

结果目录存在时默认拒绝覆盖，除非显式指定 `--force`。这比不同脚本有时覆盖、有时拒绝的行为更容易维护。

### 8.3 第三步：把拓扑和流量变成公共数据模型

建议先抽取两个小对象，而不是抽象整个 ns-3：

- `TopologyModel`：节点角色、DC、host/leaf/spine/gateway、NIC、DCI、链路带宽和时延；
- `FlowSpec`：flow id、src、dst、sport、dport、payload bytes、start time、priority 和 role。

拓扑文件可以继续使用当前格式，但需要有明确的角色/区域来源。可按以下顺序渐进改造：

1. 先把当前固定 ID 和范围集中到一个解析/校验对象；
2. 再让拓扑文件显式声明 DC 和角色，或由图结构推导并保存结果；
3. DCI 由角色或拓扑标记识别，不再由多个入口分别硬编码；
4. workload 的 DC0/DC1、query receiver 和 `PrintResults` 接收端都从模型获取；
5. Longhaul validator 与 C++ 使用同一套基础解析规则。

这样仍然可以保留当前拓扑文件，不会把“改变实验拓扑”和“重构代码”混在一次提交里。

### 8.4 第四步：抽取公共实验基础设施

当配置和数据模型稳定后，按以下顺序抽取：

1. `ConfigLoader`：类型转换、默认值、范围检查、单位和错误行号；
2. `TopologyLoader`：节点/链路读取、角色映射和 DCI 校验；
3. `AddressAndRouteBuilder`：地址分配、路由计算和路由表安装；
4. `RdmaNetworkBuilder`：Qbb、RDMA、MMU、trace 的公共初始化；
5. `FlowInstaller`：统一 flow 解析、端口分配、启动和完成统计；
6. `MetricsWriter`：CSV 表头、metadata、计数器和 schema version。

这些类只服务于 Longhaul 和 workload 两个需要共享的入口。legacy target 可以继续保留旧路径，通过适配层逐步迁移，不要求一次性重写。

### 8.5 第五步：拆分研究控制

将 `longhaul-research.h` 改为正常的接口/实现模块：

```text
ResearchController
├── ResearchConfig
├── TelemetrySampler
├── VirtualQueue / Predictor
├── CnpExecutor
└── ResearchMetrics
```

控制器通过显式 `ResearchContext` 获取：

- 选定 receiver 和 flow 列表；
- DCI 设备或抽象的链路观测接口；
- source/rx trace 注册接口；
- 控制包发送接口；
- 输出 sink。

研究控制不应再通过包含 `.h` 实现文件的方式访问 Longhaul 的所有全局对象。

### 8.6 第六步：逐步收敛模型层

模型层重构要晚于实验契约，因为模型层改动影响面最大。推荐顺序：

1. 给 `RdmaHw`、`RdmaQueuePair` 提供稳定的只读统计接口，例如 TX bytes、RX bytes、完成状态和当前速率；
2. 让示例程序不再直接访问 `m_rdma`、`m_recv_bytes` 等内部字段；
3. 把 `RdmaHw` 中的算法分支按 controller/policy 逻辑分组，但先保留现有运行路径；
4. 把 `SwitchNode` 的通用转发、采样、反馈封装和具体 FRP/Bifrost/ATC 控制分离；
5. 让 `FrpRateCalculator` 统一负责 FRP 单位换算、阈值和参数校验，避免 `SwitchNode` 再复制一套 qref/速率计算；
6. 最后才评估是否需要真正的策略插件接口。

不建议现在把每个拥塞控制算法都包装成动态插件，也不建议把所有模型类拆成大量小类。当前最有价值的是减少公开内部状态和重复单位换算。

## 9. 优先修改顺序

### P0：实验可重复性和输入输出契约

| 顺序 | 建议工作 | 目标文件范围 | 完成标准 |
|---:|---|---|---|
| 1 | 固定一条有限流量的 Longhaul smoke 流程 | `LonghaulCC` 配置、拓扑、flow、runner | 构建一次、运行一次，能核对 expected/completed flow 和 FCT，不使用 1 TB 流作为有效结果 |
| 2 | runner 不再修改 tracked 配置，所有运行生成独立 effective config | `run_scripts/`、`LonghaulCC/run-longhaul-baseline.py` | 原始配置保持不变；每次运行都有配置快照和唯一输出目录 |
| 3 | 明确 CLI/config 优先级并消除重复参数含义 | Longhaul/workload C++ 入口和 runner | `algorithm`、`cc_mode`、seed/run 只有一个最终值，metadata 中记录有效值 |
| 4 | 加入配置、flow、CDF 和拓扑启动前校验 | C++ parser、Python validator、CDF 读取路径 | 错误文件在仿真启动前失败，并给出文件、行/字段和原因 |
| 5 | 统一最小输出契约 | Longhaul 输出和 workload adapter | FCT、完成数、接收字节、stop reason 和 schema version 可机器读取 |
| 6 | 更新迁移后的文档路径，修复配置文件异常字符 | `docs/`、Longhaul README、`config-burst.txt` | 不再把 `PowerTCP/longhaul-*` 当作当前入口；配置文件可被校验器读取 |

### P1：减少重复和隐式耦合

| 顺序 | 建议工作 | 完成标准 |
|---:|---|---|
| 7 | 抽取 `ConfigLoader`、`TopologyLoader`、`FlowSpec`、`MetricsWriter` | Longhaul 和 workload 至少共享解析/输出契约 |
| 8 | 抽取公共地址、路由和 RDMA 初始化 | 两个入口的公共部分只有一份，场景差异在明确的 hook 中 |
| 9 | 将 `longhaul-research.h` 改为正常模块和显式上下文 | 研究控制不再依赖入口文件的全局变量和 include 顺序 |
| 10 | 提供模型观测 API | 实验入口不再直接访问模型内部成员 |
| 11 | 让 analyzer 完全依赖 manifest/schema | 不再用固定端口、节点号和 stdout 文本猜测结果含义 |

### P2：长期结构整理和性能优化

| 顺序 | 建议工作 | 完成标准 |
|---:|---|---|
| 12 | 合并或共享 CDF 实现，清理旧脚本和绝对路径 | 只保留一个实现，旧入口有明确兼容说明 |
| 13 | 将 `plot scripts` 重命名为无空格的目录并统一分析入口 | 批处理不依赖 shell 特殊处理 |
| 14 | 按控制器拆分 `RdmaHw`/`SwitchNode` | 每类控制器的参数和状态边界可单独测试 |
| 15 | 对采样和 flow 查找做性能测量后优化 | 以 profile 为依据，避免未测量的微优化 |

## 10. 可执行的验证门槛

每一阶段都应保留一条最小、可信、可解释的实验闭环：

1. `./ns3 show targets` 能看到目标；
2. `./ns3 build longhaul-convergence -j2` 成功；
3. 使用有限 payload 的单流或小规模流量运行 smoke；
4. 输出目录包含有效配置、输入快照、日志、metadata 和结构化 FCT；
5. expected flow count、completed flow count、FCT 行数和 receiver bytes 一致；
6. analyzer 能仅凭 manifest 和 CSV 生成结果，不依赖手工修改路径；
7. 再运行一个 PowerTCP workload smoke，确认公共改动没有破坏 legacy 入口；
8. 以上通过后，才进行多算法、多 seed 或大规模参数扫描。

建议以后把验证分成三层：

| 层级 | 内容 | 运行成本 |
|---|---|---:|
| 配置检查 | key、类型、单位、范围、路径、拓扑和 flow 引用 | 秒级 |
| smoke | 小拓扑、有限流量、短 stop time、检查输出契约 | 秒到分钟 |
| 正式实验 | 固定拓扑、payload、stop time、seed/run 和算法矩阵 | 较高 |

`exit code == 0` 只能说明进程退出，不应作为实验成功的唯一标准。

## 11. 暂时不建议做的事情

- 不要在同一阶段重写所有拥塞控制算法；否则无法区分结构改动和算法行为改变。
- 不要立即删除 PowerTCP legacy target；先保留兼容入口并记录迁移状态。
- 不要把所有配置文件合并成一个巨型配置；应先明确场景、公共参数和算法私有参数的边界。
- 不要先做复杂的插件化、反射式配置或通用工作流平台；当前问题可以用 typed config、manifest 和少量 helper 解决。
- 不要只改目录名而不更新 runner、CMake、文档、输入路径和结果说明。
- 不要把输出格式的变化和算法、拓扑变化放在同一个实验提交中。

## 12. 最终建议

建议按下面的实际顺序推进：

```text
有限流量 smoke 和有效性边界
        ↓
统一 manifest、配置优先级和每次运行目录
        ↓
统一 flow/topology/output 数据模型
        ↓
抽取 Longhaul 与 workload 的公共实验初始化
        ↓
拆分研究控制和模型观测接口
        ↓
按 profile 优化采样、查找和输出
        ↓
最后再整理目录、命名和 legacy 兼容层
```

第一优先级不是“把文件拆得更细”，而是让每次实验都能回答：

- 运行的到底是哪份配置？
- 拓扑中的节点和链路语义是什么？
- 预期有多少流，实际完成多少流？
- 输出字段的单位和统计口径是什么？
- 结果能否由另一台机器或未来的自己复现？

当这些问题有稳定答案后，公共代码抽取和模型解耦才会降低风险；否则重构只会把当前不明确的行为复制到更多文件中。

## 附：本次审查的关键证据位置

- 构建入口：`simulator/ns-3.39/examples/CMakeLists.txt`、`examples/LonghaulCC/CMakeLists.txt`、`examples/PowerTCP/CMakeLists.txt`；
- Longhaul 主入口：`simulator/ns-3.39/examples/LonghaulCC/longhaul-convergence.cc`；
- Longhaul 研究控制：`simulator/ns-3.39/examples/LonghaulCC/longhaul-research.h`；
- Longhaul 运行/分析：`run-longhaul-baseline.py`、`run-research.py`、`analyze-longhaul.py`、`plot-longhaul.py`；
- workload 入口：`simulator/ns-3.39/examples/PowerTCP/crossDC-evaluation-workload.cc`；
- 模型耦合：`simulator/ns-3.39/src/point-to-point/model/rdma-hw.*`、`rdma-queue-pair.*`、`switch-node.*`、`qbb-net-device.*`；
- 输入错误路径：`simulator/ns-3.39/examples/PowerTCP/cdf.c`、`config-burst.txt`；
- 运行编排：`run_scripts/run_single_algo.py`、`run_single_workload_algo.py`、`run_frp_simulation.py`；
- 文档和路径漂移：`docs/PROGRAM_STRUCTURE.md`、`docs/拥塞控制研究下一步TODO.md`、`docs/拥塞控制方案与初步实验报告-20260917.md`。
