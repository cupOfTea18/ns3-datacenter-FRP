# LonghaulCC 使用手册

当前完整操作说明统一见 [README](README.md)，避免场景默认值和统计口径出现两套版本。

- 默认是 S0、0.38 s 瞬态观察；单次 C++ 配置默认 DCQCN，批量 runner 默认五算法。
- 单算法入口是 `run-longhaul.py --algorithm ...`，批量入口是 `run-longhaul-all.py`。
- `--purpose transient` 报告未完成数量；`--purpose completion` 必须指定 `--stop-times`，并验收所有流。
- 缩小 S2/S3/S5 另存为 `flow-longhaul-s*-scaled.txt`，不改正式 S0–S5。
- `--r3-queue-mode history|snapshot` 是完整 R3/旧快照消融，默认 history；不是 R2 控制器。
- 使用新输出目录。有效配置、输入路径及哈希、命令和用途保存在运行目录，配置直接引用原始流量和拓扑文件。
- `--skip-build` 仅跳过编译并使用现有程序；首次运行应省略，修改 C++ 代码后需重新构建。
- 先运行 `analyze-longhaul.py --root ...`，再运行 `plot-longhaul.py --root ... --all-scenarios`。
- 全网丢弃/残留、逐流 RX 与完成状态在所有算法中采用同一记录口径。进程退出成功不等于性能验收。

流文件第一行为流数，其余每行：

```
src dst pg dport size_bytes start_time_seconds
```

拓扑、节点编号、模式对照命令、事件窗口、输出文件及结果限制均见 README。
R3 控制实现见 [PROPOSED_R3_IMPLEMENTATION.md](PROPOSED_R3_IMPLEMENTATION.md)。
