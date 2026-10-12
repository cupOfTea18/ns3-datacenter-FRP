# LonghaulCC 使用手册

当前完整操作说明统一见 [README](README.md)，避免场景默认值和统计口径出现两套版本。

- runner 和 C++ 统一读取 `config-longhaul.txt`：S0、CC proposed、0.38 s；用途默认为瞬态观察。
  单算法必须指定 `--algorithm`，批量 runner 默认五算法。
  runner 生成本次运行的 `config.txt`，通过 `--config` 直接传给 C++，不再创建 `examples/` 配置链接。
- 单算法入口是 `run-longhaul.py --algorithm ...`，批量入口是 `run-longhaul-all.py`。
- `--purpose transient` 报告未完成数量；`--purpose completion` 必须指定 `--stop-times`，并验收所有流。
- 缩小 S2/S3/S5 另存为 `flow-longhaul-s*-scaled.txt`，不改正式 S0–S5。
- 配置只保存实验设置；DCQCN/HPCC/TIMELY/Bifrost 参数在 `longhaul-convergence.cc` 的常量区修改。
- 当前 `proposed` 固定运行 R4；算法参数在 `src/point-to-point/model/dci-gateway-node.h` 的
  `DciGatewayNode::Config` 修改，默认 history、busy-observed、source-boundary、optimistic。
  修改后重新构建；runner 不再提供队列/服务模式覆盖，实际值从仿真元数据读取。
- 使用新输出目录。保留配置、`inputs/` 输入副本、结果和运行元数据；元数据保存配置/输入哈希、构建身份、命令和用途。
- `--skip-build` 要求当前源码、二进制和共享库身份匹配 runner 的上次构建记录；首次运行省略，源码变化后重新构建。
- 先运行 `analyze-longhaul.py --root ...`，再运行 `plot-longhaul.py --root ... --all-scenarios`。
- 全网丢弃/残留、逐流 RX 与完成状态在所有算法中采用同一记录口径。进程退出成功不等于性能验收。

流文件第一行为流数，其余每行：

```
src dst pg dport size_bytes start_time_seconds
```

拓扑、节点编号、模式对照命令、事件窗口、输出文件及结果限制均见 README。
R4 当前实现、配置和历史验证见 [R4_IMPLEMENTATION_VALIDATION.md](R4_IMPLEMENTATION_VALIDATION.md)。
网关已实现源侧 CE 转近端反馈和 A/B 受控出口禁用新标记；主机接收端仍将 ECT(0) 当作拥塞，
不能据此声称端到端反馈隔离成立。源码存在或功能回归记录不等于性能结论。
迁移前 R3 说明与历史记录保留在 [PROPOSED_R3_IMPLEMENTATION.md](PROPOSED_R3_IMPLEMENTATION.md)，
其中的构建和运行命令需使用对应 R3 源码环境。
