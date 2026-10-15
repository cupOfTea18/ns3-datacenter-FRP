# LonghaulCC 使用手册

当前操作说明、输入格式、构建命令和结果口径统一见 [README](README.md)。
`proposed` 当前运行 R5 方案一；实现及验证范围见 [R5 实现与验证](R5_IMPLEMENTATION_VALIDATION.md)。

- 配置保存实验设置，算法参数在 C++ 中修改；修改后重新构建。
- runner 保存实际配置、输入副本和构建身份；使用新的结果目录。
- `--purpose completion` 需要明确停止时间；验收每条流的 TX/RX、完成、丢弃和残留。
- 先分析，再绘图；网关目标速率不能当成实际吞吐。
- R4 文档是历史记录，其代码接口和验证结论不适用于当前 R5。
