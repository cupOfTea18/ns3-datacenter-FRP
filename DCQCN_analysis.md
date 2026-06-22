# DCQCN 完整实现分析

## 一、DCQCN架构概述

DCQCN (Datacenter Quantized Congestion Notification) 是 Mellanox 实现的基于ECN的拥塞控制协议，对应代码中的 `ccMode=1`。

### 核心组件
1. **Switch (交换机)**: ECN标记 + 队列监控
2. **Host (网卡/RdmaHw)**: CNP处理 + 速率调整
3. **CNP (Congestion Notification Packet)**: 拥塞通知包

---

## 二、Switch端操作流程

### 2.1 数据包入队 (switch-node.cc:180-195)
```cpp
// 1. 查路由表确定出端口
int idx = GetOutDev(p, ch);

// 2. 确定队列索引 (qIndex)
// - 控制包(CNP/PFC/ACK) -> qIndex=0 (最高优先级)
// - 有MyPriorityTag -> 使用tag指定的队列
// - 默认 -> queue 1

// 3. 入口/出口准入控制 (MMU)
if (m_mmu->CheckIngressAdmission(...) && m_mmu->CheckEgressAdmission(...)) {
    m_mmu->UpdateIngressAdmission(...);
    m_mmu->UpdateEgressAdmission(...);
} else {
    return; // Drop - 队列满则丢包
}

// 4. 检查并发送PFC (Pause Frame Control)
CheckAndSendPfc(inDev, qIndex);

// 5. 发送到出端口队列
m_devices[idx]->SwitchSend(qIndex, p, ch);
```

### 2.2 ECN标记 (需要查找具体实现)
**问题**: 代码中没有找到明确的ECN标记逻辑。

可能的实现位置：
- `qbb-net-device.cc` 的 `SwitchSend` 函数
- 在包入队时检查队列长度，超过阈值则标记ECN

### 2.3 CNP生成 (需要确认)
DCQCN中，**交换机不直接发送CNP**。流程应该是：
1. 交换机标记数据包的ECN位
2. 接收方Host检测到ECN标记的包
3. 接收方Host生成CNP并发送回发送方

**需要确认**: 接收方如何检测ECN标记并生成CNP？

### 2.4 队列长度监控 (switch-node.cc:263-271)
```cpp
void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p) {
    // 只在switch 10/13/15上输出
    if (m_id == 10 || m_id == 13 || m_id == 15) {
        static uint32_t qlenCounter = 0;
        if (qlenCounter++ % 10 == 0) {  // 每10次dequeue输出一次
            Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
            if (dev) {
                uint32_t qBytes = dev->GetQueue()->GetNBytesTotal();
                printf("[DCQCN_QLEN] %lu %u %u %u\n", 
                    Simulator::Now().GetTimeStep(), m_id, ifIndex, qBytes);
            }
        }
    }
}
```

**日志格式**: `[DCQCN_QLEN] timestep switch_id ifIndex queueBytes`
- timestep: 仿真时间步 (纳秒级)
- switch_id: 交换机ID (13是瓶颈交换机)
- ifIndex: 出端口索引 (1是瓶颈端口)
- queueBytes: 队列总字节数

---

## 三、Host端 (发送方) 操作流程

### 3.1 发送数据包 (rdma-hw.cc:662-693)
```cpp
void RdmaHw::PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap) {
    qp->lastPktSize = pkt->GetSize();
    uint32_t seq = qp->snd_nxt;
    qp->rates[qp->snd_nxt] = Simulator::Now().GetNanoSeconds();
    UpdateNextAvail(qp, interframeGap, pkt->GetSize());
}

void RdmaHw::UpdateNextAvail(...) {
    // 计算发送间隔
    Time sendingTime = interframeGap + qp->m_rate.CalculateBytesTxTime(pkt_size);
    qp->m_nextAvail = Simulator::Now() + sendingTime;
    
    // 输出TX RATE日志 (每100次采样一次)
    static uint32_t debugCounter = 0;
    if (debugCounter++ % 100 == 0) {
        std::cout << "[TX RATE] Host " << m_node->GetId() 
                  << " qp=" << qp->sip << "->" << qp->dip
                  << " m_rate=" << (qp->m_rate.GetBitRate()/1e6) << "Mbps"
                  << " ... " << std::endl;
    }
}
```

### 3.2 接收CNP (rdma-hw.cc:476-750)
```cpp
// ReceiveAck 函数中处理CNP (第476行)
if (cnp) {
    if (m_cc_mode == 1) { // DCQCN (MLX版本)
        cnp_received_mlx(qp);
    }
}

void RdmaHw::cnp_received_mlx(Ptr<RdmaQueuePair> q) {
    printf("[DCQCN_CNP] %.3fms node=%u cnp_received sip=0x%08x dip=0x%08x first_cnp=%d alpha=%.4f rate=%.3fG\n",
        Simulator::Now().GetSeconds() * 1000.0, m_node->GetId(), 
        q->sip.Get(), q->dip.Get(), q->mlx.m_first_cnp, q->mlx.m_alpha, 
        q->m_rate.GetBitRate() * 1e-9);
    
    q->mlx.m_alpha_cnp_arrived = true;
    q->mlx.m_decrease_cnp_arrived = true;
    
    if (q->mlx.m_first_cnp) {
        // 第一次收到CNP
        q->mlx.m_alpha = 1;
        q->mlx.m_alpha_cnp_arrived = false;
        
        // 调度alpha更新定时器
        ScheduleUpdateAlphaMlx(q);
        
        // 调度速率降低定时器
        ScheduleDecreaseRateMlx(q, 1);
        
        // 设置初始速率 (line rate的一定比例)
        q->mlx.m_targetRate = q->m_rate = m_rateOnFirstCNP * q->m_rate;
        q->mlx.m_first_cnp = false;
    }
}
```

### 3.3 速率调整机制 (MLX版本)

#### 3.3.1 Alpha更新 (UpdateAlphaMlx)
```cpp
// 周期性更新alpha (EWMA)
void RdmaHw::UpdateAlphaMlx(Ptr<RdmaQueuePair> q) {
    if (q->mlx.m_alpha_cnp_arrived) {
        q->mlx.m_alpha = (1 - m_g) * q->mlx.m_alpha + m_g;
    } else {
        q->mlx.m_alpha = (1 - m_g) * q->mlx.m_alpha;
    }
    q->mlx.m_alpha_cnp_arrived = false;
    ScheduleUpdateAlphaMlx(q);
}
```

#### 3.3.2 速率降低 (CheckRateDecreaseMlx)
```cpp
void RdmaHw::CheckRateDecreaseMlx(Ptr<RdmaQueuePair> q) {
    if (q->mlx.m_decrease_cnp_arrived) {
        // 有新CNP到达，降低速率
        q->mlx.m_targetRate = q->m_rate = (1 - q->mlx.m_alpha / 2) * q->m_rate;
        q->mlx.m_decrease_times++;
        
        // 重置速率增加相关状态
        q->mlx.m_rateIncTimes = 0;
        q->mlx.m_timerStarted = false;
    }
    q->mlx.m_decrease_cnp_arrived = false;
    ScheduleDecreaseRateMlx(q, m_rateDecreaseInterval);
}
```

#### 3.3.3 速率增加 (RateIncEventMlx)
```cpp
void RdmaHw::RateIncEventMlx(Ptr<RdmaQueuePair> q) {
    if (!q->mlx.m_decrease_cnp_arrived) {
        // 没有拥塞，增加速率
        if (q->mlx.m_rateIncTimes < m_rpgThreshold) {
            ActiveIncreaseMlx(q);  // AI: r = r + R_AI
        } else {
            HyperIncreaseMlx(q);   // HAI: r = r + R_HAI
        }
    }
}
```

---

## 四、现有日志分析

### 4.1 已启用的日志

#### 1. [TX RATE] - 发送方速率 (rdma-hw.cc:682-692)
**格式**:
```
[TX RATE] Host 0 qp=11.0.0.1->11.0.0.17 m_rate=25000Mbps m_max_rate=25000Mbps pkt_size=1048 snd_nxt=1000 snd_una=0 bytesLeft=999999999000 nextAvail=130000us
```

**信息**:
- Host ID
- 源IP -> 目的IP
- 当前发送速率 (m_rate)
- 最大速率 (m_max_rate)
- 包大小
- 序列号信息
- 下次可用时间

**采样率**: 每100次发送输出一次

**问题**: 
- ✅ 已修改为输出所有流（之前只输出Host 0）
- ❓ 需要验证是否真的生效

#### 2. [DCQCN_QLEN] - 交换机队列长度 (switch-node.cc:268-269)
**格式**:
```
[DCQCN_QLEN] 199996000 13 1 112136
```

**信息**:
- 时间步 (timestep)
- 交换机ID (13)
- 出端口索引 (1)
- 队列字节数 (Bytes)

**采样率**: 每10次dequeue输出一次

**问题**:
- ✅ 只在switch 10/13/15上输出
- ❓ 需要验证是否真的输出

#### 3. [DCQCN_CNP] - CNP接收 (rdma-hw.cc:734-735)
**格式**:
```
[DCQCN_CNP] 199.997ms node=5 cnp_received sip=0x0b000006 dip=0x0b000011 first_cnp=0 alpha=0.0039 rate=7.764G
```

**信息**:
- 时间 (ms)
- 节点ID
- 源IP/目的IP
- 是否首次CNP
- alpha值
- 当前速率



### 4.2 日志充分性评估

| 需求 | 现有日志 | 充分性 | 备注 |
|------|---------|--------|------|
| **发送方速率** | [TX RATE] | ⚠️ 部分 | 需要验证是否输出所有流 |
| **交换机队列长度** | [DCQCN_QLEN] | ⚠️ 部分 | 需要验证是否输出 |
| **CNP触发情况** | [DCQCN_CNP] | ✅ 充分 | 有完整信息 |
| **速率调整过程** | 无直接日志 | ❌ 不充分 | 只有CNP时的速率，缺少AI/HAI阶段 |
| **ECN标记情况** | 无 | ❌ 不充分 | 不知道哪些包被标记 |

---

## 五、需要补充的日志

### 5.1 速率调整详细日志
在以下位置添加日志：

1. **速率降低时** (CheckRateDecreaseMlx):
```cpp
printf("[DCQCN_RATE_DEC] %.3fms node=%u qp=%s old_rate=%.3fG new_rate=%.3fG alpha=%.4f\n",
    Simulator::Now().GetSeconds()*1000, m_node->GetId(), 
    qp->sip.Get(), old_rate, new_rate, q->mlx.m_alpha);
```

2. **速率增加时** (RateIncEventMlx):
```cpp
printf("[DCQCN_RATE_INC] %.3fms node=%u qp=%s old_rate=%.3fG new_rate=%.3fG type=%s\n",
    Simulator::Now().GetSeconds()*1000, m_node->GetId(),
    qp->sip.Get(), old_rate, new_rate, 
    (q->mlx.m_rateIncTimes < m_rpgThreshold) ? "AI" : "HAI");
```

### 5.2 ECN标记日志
在交换机入队/出队时检查ECN标记：
```cpp
// 在qbb-net-device.cc的SwitchSend或Enqueue函数中
if (queueBytes > ecnThreshold) {
    // 标记ECN
    printf("[DCQCN_ECN] %.3fms switch=%u port=%u qBytes=%u threshold=%u\n",
        Simulator::Now().GetSeconds()*1000, m_id, port, queueBytes, ecnThreshold);
}
```

---

## 六、总结

### 现有日志可以提供的信息：
✅ 发送方速率（需要验证）  
✅ 交换机队列长度（需要验证）  
✅ CNP接收情况  
⚠️ 速率调整结果（间接通过CNP时的速率推断）

### 现有日志缺少的信息：
❌ 速率增加过程 (AI/HAI阶段)  
❌ ECN标记触发情况  
❌ 速率降低的具体数值变化  
❌ Alpha的完整变化过程

### 建议：
1. **首先验证** [TX RATE] 和 [DCQCN_QLEN] 是否真的输出
2. 如果需要详细分析DCQCN行为，添加速率调整的详细日志
3. 如果只需要宏观性能，现有日志可能够用（但需要验证）
