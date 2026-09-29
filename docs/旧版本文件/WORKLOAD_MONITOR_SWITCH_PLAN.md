# Workload 队列监控实现说明

本文档说明两条实验轨道的交换机队列监控实现现状，以及如何修改监控的交换机/端口。早期曾有"固定监控交换机 92/32"的方案设想，但最终实现采用**不同轨道不同策略**：Track 1 固定监控瓶颈端口，Track 2 自动检测 Top 5 拥塞端口。

---

## 1. 队列日志的两条产生路径

队列长度日志由 `simulator/ns-3.39/src/point-to-point/model/switch-node.cc` 输出，共两处，对应两套算法路径：

### a) `[DCQCN_QLEN]` — DCQCN/HPCC/Timely 路径（`ccMode ∈ {1,3,7}`）

位于 `SwitchNode::SwitchNotifyDequeue()`（约第 290–300 行）：

```cpp
// DCQCN队列长度监控：输出指定交换机的出口队列长度
// 85=原瓶颈点, 32/93=新增固定监控点
if ( m_id == 32 || m_id == 85 || m_id == 93) {
    Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
    if (dev) {
        uint32_t qBytes = dev->GetQueue()->GetNBytesTotal();
        printf("[DCQCN_QLEN] %lu %u %u %u\n",
            Simulator::Now().GetTimeStep(), m_id, ifIndex, qBytes);
    }
}
```

- 触发时机：每次从出口设备出队一个报文时。
- 字段：`时间(ns)  交换机ID  端口ifIndex  队列字节数`。
- **⚠ 关键限制**：此处硬编码了 `m_id == 32 || m_id == 85 || m_id == 93`，所以日志里**只有 ToR32、ToR85、SW93** 三个交换机的队列数据。其他交换机根本不会输出 `[DCQCN_QLEN]`。

### b) `[FRP_DATA_SW]` — FRP/ROCC 路径（`ccMode ∈ {13,14}`）

位于同文件约第 701 行（FRP 反馈报文构造处）：

```cpp
double qCurKB = static_cast<double>(currentQDepth) / 1024.0;
fprintf(stderr, "[FRP_DATA_SW] %.9f %u %u %.2f %.2f %.2f %d\n",
        Simulator::Now().GetSeconds(), m_id, ifIndex,
        fairRateMbps, qCurKB, (qDevField * 600.0 / 1024.0), ccMode);
```

- 触发时机：FRP/ROCC 周期性反馈（40us）构造反馈报文时，对**所有交换机**输出。
- 字段：`时间(s)  交换机ID  端口  公平速率Mbps  队列KB  qDev  ccMode`。
- 这一路径**没有**按 `m_id` 过滤，所以所有交换机都会有数据。

---

## 2. Track 1（burst 场景）：固定监控 Switch 93 Port 1

`run_single_algo.py` 解析日志时，固定筛选 **Switch 93 Port 1** 的队列数据：

```python
if '[DCQCN_QLEN]' in l and ccMode in [1, 3, 7]:
    parts = l.split()
    if len(parts) >= 5 and int(parts[2]) == 93 and int(parts[3]) == 1:
        sw10_time.append(float(parts[1]) / 1e9 * 1000)  # ns -> ms
        sw10_q.append(float(parts[4]) / 1024.0)          # Bytes -> KB

if '[FRP_DATA_SW]' in l and ccMode in [13, 14]:
    parts = l.split()
    if len(parts) >= 8 and int(parts[2]) == 93 and int(parts[3]) == 1:
        if int(parts[7]) == ccMode:
            sw10_time.append(float(parts[1]) * 1000)    # s -> ms
            sw10_q.append(float(parts[5]))              # KB
```

- SW93 Port 1 是 `crossDC-evaluation.cc` 中 flow.txt 突发场景的瓶颈端口。
- 改监控端口：修改 `run_single_algo.py` 中 `int(parts[2]) == 93 and int(parts[3]) == 1` 的交换机 ID 与端口。
- 若新监控的交换机不在 `{32, 85, 93}` 中且使用 DCQCN 路径，还必须同时改 `switch-node.cc` 的硬编码过滤并重新构建。

---

## 3. Track 2（workload 场景）：自动检测 Top 5 拥塞端口

`run_single_workload_algo.py` 不固定监控端口，而是自动检测：

```python
MIN_SAMPLES = 5
congestion_points = []
for (sw, port), data in all_queue_data.items():
    queues = data["queues"]
    if len(queues) < MIN_SAMPLES:
        continue
    congestion_points.append({
        "switch": sw, "port": port,
        "avg_q_kb": float(np.mean(queues)),
        "max_q_kb": float(np.max(queues)),
        "n_samples": len(queues),
        ...
    })
congestion_points.sort(key=lambda x: x["avg_q_kb"], reverse=True)
top_congestion = congestion_points[:min(5, len(congestion_points))]
```

- 收集日志中所有 `(switch, port)` 对的队列数据。
- 过滤样本数 `< 5` 的对。
- 按平均队列长度降序排序，取前 5 个作为 `top_congestion` 绘图。
- 无 `--monSwitches` 参数；如需固定监控特定交换机，需修改此处的筛选逻辑。

> **注意**：DCQCN 路径下，由于 C++ 侧只输出 `{32, 85, 93}` 三个交换机，自动检测的候选集也仅限这三个交换机的端口。FRP/ROCC 路径则覆盖所有交换机。

---

## 4. 如何修改监控的交换机

### 4.1 DCQCN/HPCC/TIMELY 路径（必须改 C++）

修改 `switch-node.cc` 的 `SwitchNotifyDequeue()` 中第 295 行附近的过滤条件：

```cpp
// 当前
if ( m_id == 32 || m_id == 85 || m_id == 93) {
// 改为需要监控的交换机，例如加入 92、87
if ( m_id == 32 || m_id == 85 || m_id == 87 || m_id == 92 || m_id == 93) {
```

改完后重新构建：

```bash
cd simulator/ns-3.39 && ./ns3 build crossDC-evaluation-workload
```

### 4.2 FRP/ROCC 路径（无需改 C++）

`[FRP_DATA_SW]` 本就输出所有交换机，只需在 Python 侧筛选即可。若要固定监控，修改 `run_single_workload_algo.py` 的 `top_congestion` 选取逻辑，或仿照 Track 1 加 `sw == X and port == Y` 过滤。

### 4.3 当前实际监控的交换机与端口（参考）

| 交换机 | 角色 | 为何监控 |
|--------|------|---------|
| **SW 93** (Agg) | ToR85 上行 Agg 之一，port 1 接 ToR85 | incast 热点路径，且 `crossDC-evaluation-workload.cc` 对其做了特殊 PFC 配置 |
| **ToR 85** | host53 接入 ToR | 3 条 query flow 汇聚到 host53 的 incast 点 |
| **ToR 32** | 6 条 query flow 同源 ToR | 多流同源上行拥塞 |

详细拥塞点分析见 [TOPOLOGY_AND_CONGESTION.md](TOPOLOGY_AND_CONGESTION.md)。

---

## 5. 其他队列输出：`monitor_buffer` / `QLEN_MON_FILE`

`crossDC-evaluation(-workload).cc` 的 `monitor_buffer()` 会遍历**所有**交换机的所有端口，把队列长度分布（按 KB 分桶计数）写入 `QLEN_MON_FILE`（由 config 的 `QLEN_MON_FILE` 指定）。该文件是分布统计输出，并非 Python 绘图所用的 `[DCQCN_QLEN]`/`[FRP_DATA_SW]` 实时日志，主要用于离线分析队列分布。

`config-workload.txt` 中相关字段：

```text
QLEN_MON_FILE mix/qlen-workload.txt
ENABLE_QLEN_MON 1
QLEN_MON_START 0
QLEN_MON_END 50000000
```
