# R5 论文方案设计：基于远端状态前向重放与双环解耦的跨数据中心 RDMA 近端注入控制

## 1. 研究背景与核心问题定义

随着跨地域分布式智算（如跨数据中心大模型训练）、异地容灾备份与广域算力协同的普及，利用专用长距光纤或广域网（WAN）将 RoCEv2（RDMA over Converged Ethernet v2）扩展至数据中心互连（DCI）场景已成为迫切需求（[Inter-Data Center RDMA 综述](https://www.google.com/search?q=https%3A%2F%2Fwww.mdpi.com%2F1999-5903%2F17%2F6%2F242)）。典型跨数据中心 RDMA 传输路径具有如下五段式拓扑特征：

$$\text{发送主机 (DCQCN)} \xrightarrow{\text{源端 DCN}} \text{近端 DCI 网关 A (缓存与整形)} \xrightarrow{\text{长距链路 (WAN)}} \text{远端 DCI 网关 B (缓存与转发)} \xrightarrow{\text{目的端 DCN}} \text{接收主机 (DCQCN)}$$

在此异构架构中，数据中心内部网络（Intra-DCN）的往返时延（RTT）处于微秒级（$2\sim 20\,\mu\text{s}$），交换机采用浅缓存设计；而跨数据中心长距链路（Long-Haul DCI Link）的传播时延达数百微秒至数十毫秒，具有极大的带宽时延积（Bandwidth-Delay Product, BDP）。将现有以 [DCQCN](https://www.google.com/search?q=https%3A%2F%2Fwww.scribd.com%2Fdocument%2F937998797%2Fp523) 为代表的端到端无损 RDMA 拥塞控制直接用于跨数据中心场景时，面临以下三个根本矛盾：

1. **反馈环路时延失配与在途流量盲区（Timescale Mismatch & In-Flight Blindness）**：原生 [DCQCN](https://www.google.com/search?q=https%3A%2F%2Fwww.scribd.com%2Fdocument%2F937998797%2Fp523)、[TIMELY](https://www.google.com/search?q=https%3A%2F%2Fwww.mdpi.com%2F1999-5903%2F17%2F6%2F242) 及 [HPCC](https://www.google.com/search?q=https%3A%2F%2Frmiao.github.io%2Fassets%2Fpdf%2Fhpcc-li.pdf) 依赖端到端反馈环。当远端 DCI B 或目的端网络发生拥塞时，拥塞信号（ECN/CNP 或 RTT/INT）需跨越长距链路返回源端主机，耗时至少一个跨数据中心 $\text{RTT}_{\text{WAN}}$。在此期间，长链路上已存在高达 $1\,\text{BDP}$ 的在途（In-Flight）失控流量持续涌入远端 DCI B，极易击穿 B 的浅缓存并触发长距 PFC 暂停风暴，或因长时延下持续生成的过量 CNP 导致发送端速率过度削减与锯齿振荡（[LSCC](https://www.google.com/search?q=http%3A%2F%2Fstaff.ustc.edu.cn%2F%7Ekpxue%2Fpaper%2FIWQoS24-MinfeiLong.pdf)、[Spillway](https://www.google.com/search?q=https%3A%2F%2Farxiv.org%2Fhtml%2F2605.11852v2)）。
2. **近端代发 CNP 与网关整形的“双重控制（Double Control）”悖论**：近期分段拥塞控制方案（如 [LSCC](https://www.google.com/search?q=http%3A%2F%2Fstaff.ustc.edu.cn%2F%7Ekpxue%2Fpaper%2FIWQoS24-MinfeiLong.pdf)、[BiCC](https://www.google.com/search?q=https%3A%2F%2Fieeexplore.ieee.org%2Fdocument%2F10621412%2F)、[GRC](https://www.google.com/search?q=https%3A%2F%2Fzhangmenghao.github.io%2Fpapers%2FICNP2026-GRC.pdf)）尝试在近端边界交换机/网关处根据远端反馈速率构造“伪队列（Pseudo-Queue）”或虚拟字节差，直接向源端主机代发近端 CNP，依靠商业 RNIC 的 DCQCN 降速来间接实现跨域限速。然而，商业 RNIC 的 DCQCN 采用 AIMD 启发式迭代（每 $50\,\mu\text{s}$ 乘法降速、依靠定时器慢速加性增），无法精确、即时地跟踪远端可用带宽的快速变化。若近端网关 A 一方面对出境流量执行精确速率整形，另一方面又根据同一个远端拥塞状态向源端主机高频发送近端 CNP，会导致网关整形器与主机 RNIC 对同一流实施双重惩罚——主机被过度压制，待远端拥塞解除时，A 自身缓存早已排空且主机供给不足，造成长链路带宽饥饿。
3. **远端出口自限速悖论与静态份额浪费**：当拥塞发生在远端 DCI B 的下游出口端口时，瓶颈本身就是 B 的出口转发能力；若此时在 B 处对该出口人为施加限速，不仅无法缓解拥塞，反而直接削减了 B 的实际排空能力（自限速悖论）。此外，若在近端网关 A 按活跃流数量静态均分远端带宽或依赖复杂的显式需求注册协议（如 R4 的 DEMAND 握手机制），不仅引入额外信令开销与启动延迟，且当某条流因源端 RNIC 限速而暂时供给不足时，其未用完的带宽配额无法被其他有积压的同组流利用，破坏了工作保守性（Work-Conservation）。

**R5 的核心研究问题**：在不修改商用端主机 RNIC 硬件协议栈、不引入显式流需求注册与完成通知协议的前提下，如何通过近端 DCI A 与远端 DCI B 的轻量化协同，既在长距链路上实现对远端排队状态的超前精准控制，又实现近端缓存保护与远端流量整形的彻底解耦？

## 2. 草稿审阅：有效设计与关键逻辑/数学缺陷修正

在正式给出 R5 完整方案前，对《R5 设计方案草稿》中的假设、公式与机制进行严格审查，保留其核心架构洞察，并修正其中存在的 **7 项关键逻辑与数学一致性缺陷**。

| 审阅维度 | 草稿原始设计与假设 | 发现的逻辑/数学缺陷与隐患 | R5 完善方案的修正与替代设计 |
| :--- | :--- | :--- | :--- |
| **1. 预测时序跨度与时钟同步假设** | 从反馈队列 $\hat{Q}_B(s)=Q_B(s)$ 出发，用 $\Delta \hat{A}_B(u)=X_A(u+\Delta-d)-X_A(u-d)$ 递推至 $\hat{Q}_B(t+d)$。 | ① **时序跨度漏算反向时延**：时刻 $t$ 收到的最新反馈产生于历史时刻 $s \le t - d_{BA}$，而时刻 $t$ 改变速率需在 $t + d_{AB}$ 才到达 B。因此需预测的总跨度为 $(t + d_{AB}) - s \ge d_{BA} + d_{AB} = \text{RTT}_{AB}$，对应 A 在 $[s-d_{AB}, t]$ 内发出的全部在途流量，而非仅单向时延 $d$。<br>② **隐含严格时钟同步假设**：直接计算 $u-d$ 要求 A 与 B 物理时钟微秒级同步且单向时延恒定已知。 | 提出**免时钟同步的锚点回显重放机制（Anchor-Echoed Replay）**：A 在出境数据包/探测帧中携带本地发送时间戳 $\tau_{\text{tx}}$ 与组累计发送字节 $X_{A,p}$；B 生成反馈时直接回显截至采样时刻 $s$ 收到的最新包锚点 $\tau_{\text{anchor}} = s - d_{AB}$ 与累计接收字节 $Y_{B,p}(s)$。A 直接在本地时钟域对 $[\tau_{\text{anchor}}, t]$ 的实际发送历史分段重放，彻底消除跨域时钟偏移与反向时延抖动影响。 |
| **2. 远端观测点与交叉流量归因** | 笼统定义远端队列 $Q_B(s)$ 与预计服务速率 $\hat{\mu}_B(u)$，又在后文提及按 B 出口端口分组。 | **状态口径与到达项错配**：B 的拥塞发生在指向目的端 DCN 的各个**出口端口 $p$**（或下游 PFC 反压对应的端口队列 $Q_{B,p}$）。端口 $p$ 除了接收来自 A 的流量外，还可能汇聚其他 DCI 站点或目的端本地交叉流量 $\lambda_{B,p}^{\text{cross}}$。若到达项只算 A 的发送量，而服务项代入端口 $p$ 的总物理发送速率 $\mu_{B,p}^{\text{out}}$，会漏算交叉流量占用的带宽，导致系统性低估 $\hat{Q}_{B,p}$。 | 将状态空间严格定义为**远端 DCI B 的每个出口拥塞域/端口 $p$**。B 分别测量端口 $p$ 的总出队速率 $\mu_{B,p}^{\text{out}}$、来自 A 的到达字节与非 A 交叉流量到达速率 $\lambda_{B,p}^{\text{cross}}$。在分段重放中，显式计入交叉流量到达并扣除总出队速率，定义可供 A 使用的**净有效服务率** $\hat{C}_{\text{avail},p} = [\mu_{B,p}^{\text{out}} - \lambda_{B,p}^{\text{cross}}]^+$。 |
| **3. 递推公式笔误与控制律鲁棒性** | 递推式写作 $\hat{Q}_B(u+\Delta)=[\hat{Q}_B(u)+\Delta\hat{A}_B(u)-\hat{\mu}_B(u)\Delta]$；控制律采用前馈加比例排空项。 | ① 递推式右括号遗漏正部算子符号 $^+$。<br>② 未给出目标队列 $Q_B^*$ 与排空时间常数 $T_{\text{drain}}$ 的稳定边界：若 $Q_B^*$ 过小，交叉流量撤出时在 1 个 $\text{RTT}_{AB}$ 反应期内队列会排空失速；若 $T_{\text{drain}}$ 过小，在业务速率估计存在误差 $\tilde{\mu}_B$ 时会引发超调振荡。 | ① 补全非线性截零算子 $[x]^+$。<br>② 基于时滞系统 [Smith 预测器](https://www.google.com/search?q=http%3A%2F%2Fcongres.cran.univ-lorraine.fr%2F2001%2FECC_2001%2Farticles%2Fecc3931.pdf) 与 [XCP](https://www.google.com/search?q=https%3A%2F%2Fconferences.sigcomm.org%2Fsigcomm%2F2002%2Fpapers%2Fxcp.pdf) 理论，导出理想匹配下闭环传递函数的一阶指数收敛性质，并给出存在估计误差时 $T_{\text{drain}} \ge \max(2\Delta_{\text{fb}}, \alpha_{\text{rob}}\text{RTT}_{AB})$ 与抗排空目标队列 $Q_{B,p}^*$ 的参数整定准则，以及带抗饱和受限积分（FF+PI）和反馈超时衰减的备选增强律。 |
| **4. 组内单流工作保守分配机制缺失** | 提出“共享 B 同一出口的流划为一组，空闲或受限流不占固定份额”，但未给出组速率如何落地到单流。 | 若将组速率 $R_{A,p}^{\text{cmd}}$ 静态除以流数配置给单流整形器，则供给不足流的剩余配额无法即时让渡给其他积压流，违背工作保守原则；若无流级隔离，单条恶意大流又会饿死同组其他流。 | 设计**两级分层工作保守整形架构（Hierarchical Work-Conserving Shaper）**：外层为绑定组速率 $R_{A,p}^{\text{cmd}}(t)$ 的组级令牌桶整形器；内层为组内活跃流间的**赤字轮询（DRR）公平调度器**。供给不足流自动让出令牌，积压流按公平份额共享组级带宽，且预测器仅需维护组级出队历史，复杂度为 $O(|\mathcal{P}_B|)$。 |
| **5. 短流线速启动的盲目性与 Incast 风险** | “短流先线速启动把数据都发出去；长流（发送时间 > 1 RTT）等 B 反馈后再调整速率。” | ① 去掉 R4 需求注册后，A 在流到达之初**无法预知流大小**。<br>② 若允许所有首 RTT 内的流绕过组级整形器以物理线速 $C_A$ 并发注入，当突发高并发短流（Incast）指向已拥塞的 B 端口 $p$ 时，会在 1 个 RTT 内注入高達 $1\,\text{BDP}$ 的失控流量冲垮 B。<br>③ 短流若绕过发送历史记账，会破坏预测器准确性。 | 改为**无先验字节阈值分级 + 组预算内短流优先借用**：<br>① 所有流（含短流）的出队字节 100% 计入组发送历史 $X_{A,p}(t)$；<br>② 累计发送字节 $B_i < \Theta_{\text{short}}$ 的流处于“短流态”，在组级整形器 $R_{A,p}^{\text{cmd}}(t)$ 内部享有高优先级（带防饥饿上限 $\gamma_{\text{short}}$），既实现零等待优先排空，又严格受组级安全预算约束；仅当组 $p$ 处于空闲冷启动时允许在安全突发配额 $B_{\text{safe}}$ 内线速启动。 |
| **6. 近端 CNP 触发与抑制条件未形式化** | “A 根据本地队列、输入速率和实际输出速率决定是否发 CNP；若主机已明显降速、队列正在排空则停止追加。” | 缺乏明确数学判据：当 A 的转发速率因远端拥塞下调时，本地队列 $Q_{A,i}$ 升高；若仅按队列阈值持续发 CNP，即便主机输入率已低于 A 转发率（$\dot{Q}_{A,i} < 0$），主机仍会被连续乘法削减至接近零，导致后续断流。 | 提出**水位与一阶导数双重门控（Level + Derivative Gated Proxy CNP）**：仅当本地流队列预计越界（$Q_{A,i} > K_A^{\min}$）**且**净流入率未达预期排空斜率（$\dot{Q}_{A,i} = \lambda_{A,i}^{\text{in}} - \mu_{A,i}^{\text{out}} > -\kappa_{\text{drain}}(Q_{A,i} - K_A^{\text{target}})$）时才按最小间隔 $\Delta_{\text{CNP}}^{\min}$ 发送近端 CNP；一旦 $\dot{Q}_{A,i}$ 进入稳定负斜率排空区，立即闭锁 CNP。 |
| **7. 原生 ECN/CNP 透传的双重控制冲突** | “先保留透传，不直接全部截断，也不清除沿途既有的 CE 标记。” | ① 若远端 B 的受控出口队列仍开启低阈值 ECN 标记，当 R5 正常将 B 队列维持在 $Q_{B,p}^* > 0$ 时，B 会持续打 CE 标记并由接收端产生原生 CNP 压垮发送主机，使 A 的整形完全失效（[LSCC](https://www.google.com/search?q=http%3A%2F%2Fstaff.ustc.edu.cn%2F%7Ekpxue%2Fpaper%2FIWQoS24-MinfeiLong.pdf) 已证实该冲突）。<br>② 若源端交换机打了 CE，A 代发近端 CNP 后不清除 CE，接收端会再回传原生 CNP 造成二次降速（[Spillway](https://www.google.com/search?q=https%3A%2F%2Farxiv.org%2Fhtml%2F2605.11852v2)、[GRC](https://www.google.com/search?q=https%3A%2F%2Fzhangmenghao.github.io%2Fpapers%2FICNP2026-GRC.pdf)）。 | 确立**按拥塞发生位置的 ECN/CNP 空间职责划分原则**：<br>① **远端 B 受控出口队列**：关闭 ECN 标记（或将阈值调至预测安全上界之上），由 A-B 预测整形环独占控制；<br>② **目的端域内交换机（B 下游）**：保留 ECN 标记与原生 CNP 透传，用于处理 B 下游的最后一跳域内拥塞；<br>③ **源端域内交换机（A 上游）**：若 A 拦截上游 CE 并已代发近端 CNP，则转发前清除该包的 CE 位，避免端到端重复惩罚。 |

## 3. R5 整体设计思想与架构分工

### 3.1 核心设计哲学：时空解耦与预测前置

R5 的核心思想可以概括为**“远端精确测量、近端超前预测与直接整形、本地导数门控反压”**。我们将原本横跨“源端主机—源端 DCN—长距链路—目的端 DCN—接收主机”的单一长时延病态闭环，解耦为三个职责正交、时间尺度匹配的协同机制：

1. **跨域预测整形主环（A $\leftrightarrow$ B，时间尺度：$\text{RTT}_{AB}$，控制执行点：近端 DCI A 出境整形器）**：
   远端 DCI B 不做自毁式限速，而是作为高精度的“远端状态传感器”，周期性向近端 DCI A 反馈各目的出口端口的真实队列长度、实际排水速率与收发字节锚点。近端 DCI A 扮演“状态观测器 + 精确执行器”：它从 B 的历史真实状态出发，将本地记录的**真实已出队历史流量**沿时间轴分段前向重放，预测出“当前时刻 $t$ 改变发送速率后、首批受控比特在 $t + d_{AB}$ 到达 B 时”的远端未来队列 $\hat{Q}_{B,p}(t + d_{AB})$，并直接调整 A 处的硬件/软件整形速率，将远端队列平稳锚定在目标工作点 $Q_{B,p}^*$。
2. **源端缓存保护反压次环（发送主机 $\leftrightarrow$ A，时间尺度：$\text{RTT}_{\text{src}} \ll \text{RTT}_{AB}$，控制执行点：A 的近端 CNP 生成器）**：
   近端 DCI A 利用自身缓存吸收主机降速滞后带来的瞬态流量差。近端 CNP **完全剥离远端拥塞控制职责**，仅作为保护 A 本地缓存不溢出的“安全阀”。只有当主机输入速率持续高于 A 的整形出队速率、导致 A 本地缓存水位超标且未处于有效排空状态时，A 才向对应 QP 发送近端 CNP。因此，商用 DCQCN 主机只需被粗粒度约束在“长期平均发送率与 A 的转发率大体匹配”的范围内，无需精确跟踪 A 的每一次微秒级速率调整。
3. **目的端域内拥塞保护机制（B $\leftrightarrow$ 目的端 DCN）**：
   - **情况 A（瓶颈在远端 DCI B 自身的出口端口 $p$）**：B 绝不人为降低端口 $p$ 的出队速率，始终保持最大工作保守转发能力，由近端 DCI A 通过预测整形减少注入长链路的流量。
   - **情况 B（瓶颈在 B 下游的目的端域内交换机，如目的 ToR Incast）**：
     - 若下游拥塞通过链路级流控（如 PFC Pause）反压至 B 的出口端口 $p$，其影响会直接体现为 B 出口端口实际出队速率 $\mu_{B,p}^{\text{out}}$ 下降与队列 $Q_{B,p}$ 增长，自动被 A $\leftrightarrow$ B 预测整形主环捕获并消解；
     - 若下游拥塞仅触发目的端内部交换机 ECN 标记而未暂停 B 的出口端口，在当前基础版本中，通过透传目的端接收主机返回的原生 CNP 通知源端（并在后续演进版本中支持 B 处基于目的端拥塞信号的按路径/VOQ 局部限速，如 [BiCC](https://www.google.com/search?q=https%3A%2F%2Fieeexplore.ieee.org%2Fdocument%2F10621412%2F) 与 [RiCE](https://www.google.com/search?q=https%3A%2F%2Fcs.nju.edu.cn%2F_upload%2Ftpl%2F01%2F65%2F357%2Ftemplate357%2Flunwen%2F2026%2Fiwqos26_RiCE.pdf)）。

### 3.2 各组件观测、决策与控制协同表

| 机制模块 | 部署位置 | 观测输入信息 | 控制动作输出 | 决策频率与生效延迟 | 协同与边界约束 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **远端遥测与反馈生成** | 远端 DCI B（按出口端口 $p$） | 瞬时队列 $Q_{B,p}(s)$、总发出字节、总收到字节、来自 A 的收到字节、最新收到 A 报文的本地发送时间戳锚点 $\tau_{\text{anchor}}$ | 向 A 发送高优先级控制反馈帧（携带 $p, Q_{B,p}(s), \mu_{B,p}^{\text{out}}, \lambda_{B,p}^{\text{cross}}, Y_{B,p}, \tau_{\text{anchor}}$） | 周期 $\Delta_{\text{fb}}$（如 $50\sim 200\,\mu\text{s}$）或队列突变事件触发；经 $d_{BA}$ 到达 A | B 自身出口队列禁用低阈值 ECN 标记，不因自身积压而限速 |
| **前向状态重放与预测器** | 近端 DCI A（按远端出口组 $p$） | B 的最新反馈包 + A 本地按微时间步 $\Delta\tau$ 记录的组级真实出队字节序列 $\{X_{A,p}(\tau_k)\}$ | 预测 $t + d_{AB}$ 时刻 B 的端口队列 $\hat{Q}_{B,p}(t+d_{AB})$ 及净可用服务率 $\hat{C}_{\text{avail},p}(t)$ | 每次收到新反馈时重锚定校正，并在每个本地控制周期 $\Delta_c$ 增量推演 | 必须使用 A 真实出队字节（而非目标配置速率），按步长 $\Delta\tau$ 逐段做 $[x]^+$ 截零 |
| **组级速率控制与分层调度** | 近端 DCI A（出境流量管理器） | 预测队列 $\hat{Q}_{B,p}(t+d_{AB})$、目标队列 $Q_{B,p}^*$、净可用容量 $\hat{C}_{\text{avail},p}(t)$、组内各流本地子队列状态 | 更新组 $p$ 整形速率上限 $R_{A,p}^{\text{cmd}}(t)$；组内短流优先、长流按 DRR 工作保守调度出队 | 计算后在 A 出境端口**立即生效（零控制滞后）**，经 $d_{AB}$ 后改变 B 的到达率 | 空闲/受限流自动让出组令牌；所有组总速率受长链路容量 $C_A$ 约束 |
| **导数门控近端 CNP 反压** | 近端 DCI A（入向/缓存监控器） | 单流（及总池）本地缓存 $Q_{A,i}(t)$、本地入速率 $\lambda_{A,i}^{\text{in}}(t)$、本地实际出速率 $\mu_{A,i}^{\text{out}}(t)$ | 向满足“水位超标且未在有效排空”的源端主机 QP 发送标准 RoCEv2 CNP | 监控周期 $\Delta_A$（如 $10\sim 50\,\mu\text{s}$）；经源端 $\text{RTT}_{\text{src}}$（微秒级）使 RNIC 降速 | 与远端预测拥塞解耦；单 QP 发送间隔 $\ge \Delta_{\text{CNP}}^{\min}$ |

## 4. 核心机制设计与数学推导

### 4.1 符号系统与量纲定义

为保证全方案数学与物理量纲严格一致，统一采用**字节（Bytes, $\text{B}$）**作为数据量与队列长度单位，**秒（Seconds, $\text{s}$）**作为时间单位，**字节每秒（$\text{B/s}$）**作为速率单位：

| 符号 | 物理含义 | 单位 | 观测/维护位置 |
| :--- | :--- | :--- | :--- |
| $p \in \mathcal{P}_B$ | 远端 DCI B 的下游出口端口（即竞争同一远端出口带宽的流聚合组索引） | 无量纲 | A 与 B 共享映射 |
| $d_{AB}, d_{BA}$ | A 到 B 的前向单向传播时延、B 到 A 的反馈单向时延，$\text{RTT}_{AB} = d_{AB} + d_{BA}$ | $\text{s}$ | 链路物理属性 |
| $C_A, C_{B,p}$ | 近端 A 长链路端口物理速率上限、远端 B 出口端口 $p$ 的物理速率上限 | $\text{B/s}$ | A / B 配置常量 |
| $Q_{B,p}(s)$ | 远端时刻 $s$ 时，B 的出口端口 $p$ 处的真实积压队列长度 | $\text{B}$ | B 直接测量 |
| $Y_{B,p}(s)$ | 截至远端时刻 $s$，B 在端口 $p$ 累计收到的来自 A 的总字节数 | $\text{B}$ | B 直接测量 |
| $\tau_{\text{anchor}}$ | 截至远端时刻 $s$，B 收到的来自 A 的最后一个数据包所携带的 A 本地发送时间戳（满足 $\tau_{\text{anchor}} = s - d_{AB}$） | $\text{s}$ | A 写入、B 回显 |
| $\mu_{B,p}^{\text{out}}(s)$ | B 出口端口 $p$ 在采样窗 $[s-\Delta_{\text{fb}}, s]$ 内的真实平均出队（发送）吞吐速率 | $\text{B/s}$ | B 直接测量 |
| $\lambda_{B,p}^{\text{cross}}(s)$ | B 出口端口 $p$ 在采样窗 $[s-\Delta_{\text{fb}}, s]$ 内收到的非 A 来源交叉流量到达速率 | $\text{B/s}$ | B 直接测量 |
| $\hat{C}_{\text{avail},p}(t)$ | A 估计的远端端口 $p$ 可供 A 流量使用的净服务能力，$\hat{C}_{\text{avail},p} = [\hat{\mu}_{B,p}^{\text{out}} - \hat{\lambda}_{B,p}^{\text{cross}}]^+$ | $\text{B/s}$ | A 维护与滤波 |
| $X_{A,p}(\tau)$ | 截至 A 本地时刻 $\tau$，A 实际向长链路发出的属于组 $p$ 的累计出队字节数 | $\text{B}$ | A 本地精确记录 |
| $\hat{Q}_{B,p}(t + d_{AB})$ | A 在本地时刻 $t$ 预测的、当 $t$ 时刻的新速率首次到达 B 时的远端端口 $p$ 队列长度 | $\text{B}$ | A 状态预测器输出 |
| $Q_{B,p}^*, T_{\text{drain}}$ | 远端端口 $p$ 的目标维持队列长度（$\text{B}$）、消除队列偏差的目标排空时间常数（$\text{s}$） | $\text{B}, \text{s}$ | 控制器超参数 |
| $Q_{A,i}(t), \lambda_{A,i}^{\text{in}}(t), \mu_{A,i}^{\text{out}}(t)$ | 流 $i$ 在近端 A 处的本地队列长度（$\text{B}$）、主机流入速率（$\text{B/s}$）、A 实际转发速率（$\text{B/s}$） | $\text{B}, \text{B/s}$ | A 本地监控 |

### 4.2 机制一：免时钟同步的远端状态测量与分段历史重放预测器

#### 4.2.1 为什么必须预测 $t + d_{AB}$ 时刻的远端队列？
设近端 DCI A 在本地当前时刻 $t$ 决定将组 $p$ 的发送速率调整为 $R_{A,p}^{\text{cmd}}(t)$。由于长链路存在前向传播时延 $d_{AB}$，在未来时间段 $u \in [s, \, t + d_{AB})$（其中 $s$ 为最新收到反馈包在 B 处的采样时刻）内到达 B 的数据包，**全部已经在时刻 $t$ 之前由 A 发送到长链路上了**。A 在时刻 $t$ 的任何控制动作，最早只能在远端时刻 $u = t + d_{AB}$ 开始改变 B 的入队速率。

因此，如果 A 根据滞后的反馈队列 $Q_{B,p}(s)$ 甚至当前时刻估计队列 $\hat{Q}_{B,p}(t)$ 来决策，就会忽略正在光纤上飞行的、时长达 $\text{RTT}_{AB}$ 的在途流量对远端队列的改变，从而引发严重的相位滞后与周期性过调。根据控制理论中的 [Smith 预测原理（Smith Principle）](https://www.google.com/search?q=http%3A%2F%2Fcongres.cran.univ-lorraine.fr%2F2001%2FECC_2001%2Farticles%2Fecc3931.pdf)，最优无滞后控制器必须以**受控动作首次到达被控对象时的预测状态 $\hat{Q}_{B,p}(t + d_{AB})$** 作为反馈输入。

#### 4.2.2 远端 DCI B 的反馈内容与无时钟偏移锚定（Anchor-Echoed Alignment）
草稿中直接写 $\Delta \hat{A}_B(u) = X_A(u+\Delta-d) - X_A(u-d)$，这在工程上隐含了“A 与 B 具有严格同步的物理时钟且 $d$ 完全已知”的强假设。若 A、B 时钟存在偏差 $\delta_{\text{clk}}$，则历史索引窗口整体平移，导致重放字节数严重失真。

**R5 替代设计**：利用因果回显消除时钟同步依赖。
1. **A 侧打标与历史记录**：A 将本地时间轴划分为步长为 $\Delta\tau$（例如 $\Delta\tau = 5\sim 10\,\mu\text{s}$）的微时间槽，在环形缓冲区（Ring Buffer，覆盖时长不少于 $\text{RTT}_{AB}^{\max} + 2\Delta_{\text{fb}}$）中记录每个组 $p$ 的真实累计出队字节 $X_{A,p}(\tau_k)$。同时，A 在发往 B 的数据包扩展头（或每隔 $\Delta\tau$ 插入的极轻量带内遥测标记）中写入 A 当前的本地时间戳 $\tau_{\text{tx}}$ 及组累计发送字节 $X_{A,p}(\tau_{\text{tx}})$。
2. **B 侧测量与回显**：B 每隔周期 $\Delta_{\text{fb}}$（或在队列跨越关键水位时）对每个活跃出口端口 $p$ 生成反馈：
   - 测量当前真实队列长度 $Q_{B,p}(s)$；
   - 计算本周期端口 $p$ 的实际总出队吞吐速率 $\mu_{B,p}^{\text{out}}(s) = \frac{\text{TxBytes}_{B,p}(s) - \text{TxBytes}_{B,p}(s - \Delta_{\text{fb}})}{\Delta_{\text{fb}}}$；
   - 计算本周期非 A 注入的交叉流量到达速率 $\lambda_{B,p}^{\text{cross}}(s) = \frac{\Delta\text{RxBytes}_{B,p}^{\text{total}}(s) - \Delta Y_{B,p}(s)}{\Delta_{\text{fb}}}$；
   - **回显锚点**：提取截至时刻 $s$ 在端口 $p$ 收到的最后一个来自 A 的数据包所携带的 A 本地发送时间戳 $\tau_{\text{anchor}}$ 与累计发送量记录（或 B 侧累计接收量 $Y_{B,p}(s)$）。

根据电磁波传播的因果性，B 在时刻 $s$ 刚收到的来自 A 的数据包，恰恰就是 A 在本地时刻 $\tau_{\text{anchor}} = s - d_{AB}$ 发出的包！因此，当该反馈历经反向时延 $d_{BA}$ 在 A 本地时刻 $t_{\text{rx}}$ 到达 A，且 A 在当前时刻 $t \ge t_{\text{rx}}$ 执行预测时：
- 远端时间区间 $u \in [s, \, t + d_{AB}]$ 与 A 的本地历史发送时间区间 $\tau \in [\tau_{\text{anchor}}, \, t]$ 形成**严格的保距同构映射**：$u(\tau) = \tau + d_{AB}$！
- A **完全不需要知道 B 的物理时钟 $s$，甚至不需要测量 $d_{AB}$ 和 $d_{BA}$ 的绝对值**，只需在本地历史环形缓冲区中从锚点时刻 $\tau_0 = \tau_{\text{anchor}}$ 一路重放到当前本地时刻 $\tau_M = t$，即可精确得到远端在 $t + d_{AB}$ 时刻的预测队列！

#### 4.2.3 考虑非线性排空边界与交叉流量的分段前向重放公式
设 A 的本地重放区间 $[\tau_{\text{anchor}}, \, t]$ 被划分为 $M$ 个微时间步：$\tau_0 = \tau_{\text{anchor}} < \tau_1 < \dots < \tau_M = t$，步长 $\Delta_k = \tau_{k+1} - \tau_k$。

- **初值校正（Anchor Initialization）**：
  在重放起点 $\tau_0 = \tau_{\text{anchor}}$（对应远端测量时刻 $s$），将预测队列初值锚定为反馈中的真实队列：
  $$\hat{Q}_{B,p}^{[0]} = Q_{B,p}(s)$$
  > **丢包自愈校验（Loss-Resilient Correction）**：若长链路为有损链路，比较 A 在 $\tau_{\text{anchor}}$ 记录的累计发送字节 $X_{A,p}(\tau_{\text{anchor}})$ 与 B 反馈的累计接收字节 $Y_{B,p}(s)$。两者之差反映历史链路丢包，而区间 $[\tau_{\text{anchor}}, t]$ 内的在途增量完全由增量差分 $\Delta X_{A,p}(\tau_k) = X_{A,p}(\tau_{k+1}) - X_{A,p}(\tau_k)$ 决定，天然消除了累积计数器漂移。

- **分段递推演化（Step-by-Step Non-Linear Forward Replay）**：
  对于 $k = 0, 1, \dots, M-1$，A 在本地历史时间段 $[\tau_k, \tau_{k+1}]$ 内**真实出队**的组 $p$ 字节数为：
  $$\Delta A_{A\to B, p}(\tau_k) = X_{A,p}(\tau_{k+1}) - X_{A,p}(\tau_k)$$
  该批数据将在远端时间段 $[s + (\tau_k - \tau_0), \, s + (\tau_{k+1} - \tau_0)]$ 到达 B 的出口端口 $p$。结合远端端口 $p$ 的预计总出队服务速率 $\hat{\mu}_{B,p}^{\text{out}}$ 与交叉流量到达速率 $\hat{\lambda}_{B,p}^{\text{cross}}$（由最新反馈平滑估计得到），远端队列按步长 $\Delta_k$ 向前递推：
  $$\hat{Q}_{B,p}^{[k+1]} = \left[ \hat{Q}_{B,p}^{[k]} + \Delta A_{A\to B, p}(\tau_k) + \hat{\lambda}_{B,p}^{\text{cross}} \cdot \Delta_k - \hat{\mu}_{B,p}^{\text{out}} \cdot \Delta_k \right]^+$$
  其中 $[x]^+ = \max(x, 0)$。若定义端口 $p$ 可供 A 使用的预计净服务速率为 $\hat{\mu}_{B,p}^{\text{net}} = \hat{\mu}_{B,p}^{\text{out}} - \hat{\lambda}_{B,p}^{\text{cross}}$，则上式可简洁写为：
  $$\hat{Q}_{B,p}^{[k+1]} = \left[ \hat{Q}_{B,p}^{[k]} + \Delta A_{A\to B, p}(\tau_k) - \hat{\mu}_{B,p}^{\text{net}} \cdot \Delta_k \right]^+$$
  递推至终点 $k = M$ 时，所得状态即为目标预测队列：
  $$\hat{Q}_{B,p}(t + d_{AB}) \triangleq \hat{Q}_{B,p}^{[M]}$$

#### 4.2.4 为什么草稿强调的三条工程铁律至关重要？（数学合理性证明）
1. **必须记录真实出队量 $\Delta X_{A,p}(\tau_k)$ 而非目标速率 $R_{A,p}^{\text{cmd}}(\tau_k)\Delta_k$**：
   由于 Jensen 不等式与供给约束，实际出队率满足 $\dot{X}_{A,p}(\tau) = \min\big(R_{A,p}^{\text{cmd}}(\tau), \, \text{Supply}_{A,p}(\tau), \, C_A^{\text{sched}}(\tau)\big) \le R_{A,p}^{\text{cmd}}(\tau)$（例如主机被 DCQCN 压低、A 本地队列排空、或长链路被高优控制帧抢占）。若像 [Bifrost](https://www.google.com/search?q=https%3A%2F%2Fcs.nju.edu.cn%2Ftianchen%2Flunwen%2F2024%2Fton24-chengyuan.pdf) 或 [LSCC](https://www.google.com/search?q=http%3A%2F%2Fstaff.ustc.edu.cn%2F%7Ekpxue%2Fpaper%2FIWQoS24-MinfeiLong.pdf) 那样在远端用下发指令速率估算在途流量，一旦上游供给不足，预测队列就会虚高（Phantom Queue Inflation），导致控制器进一步错误压低速率，陷入死锁式欠载。
2. **必须按微时间步 $\Delta_k$ 逐步取 $[x]^+$，而不能对整个 $\text{RTT}_{AB}$ 窗口一次性求总和再截零**：
   因为算子 $f(x) = [x]^+$ 是凸函数而非线性函数：
   $$\left[ \left[ Q_0 + \Delta_1 \right]^+ + \Delta_2 \right]^+ \ge \left[ Q_0 + \Delta_1 + \Delta_2 \right]^+$$
   若在前半段 $\tau \in [\tau_0, \tau_{M/2}]$ 内 A 未发送数据（$\Delta_1 < 0$，远端队列早已排空至 $0$，剩余服务能力被浪费而无法“存成负队列”），而在后半段 $\tau \in [\tau_{M/2}, \tau_M]$ 内 A 突发发送了大量数据（$\Delta_2 > 0$），一次性求和会将前半段空转浪费的服务能力错误用于抵消后半段的突发积压，从而严重**低估** $t + d_{AB}$ 时刻的真实队列，诱发远端缓存溢出！
3. **新反馈到达时必须从 $\tau_{\text{anchor}}$ 重新校正并重放 $[\tau_{\text{anchor}}, t]$，绝不能直接令 $\hat{Q}_{B,p}(t+d_{AB}) \leftarrow Q_{B,p}(s)$**：
   $Q_{B,p}(s)$ 反映的是距目标时刻 $t + d_{AB}$ 整整早了 $\Delta t_{\text{age}} + \text{RTT}_{AB}$ 的历史状态。直接覆盖相当于丢弃了整个在途窗口的动态记忆，退化为无预测的纯滞后反馈控制。

### 4.3 机制二：近端 DCI A 的预测速率控制律与分层工作保守调度

#### 4.3.1 预测比例前馈控制律（FF+P）及其闭环稳定性分析
在获得预测队列 $\hat{Q}_{B,p}(t + d_{AB})$ 与预计净可用服务能力 $\hat{C}_{\text{avail},p}(t) = \left[\hat{\mu}_{B,p}^{\text{out}}(t) - \hat{\lambda}_{B,p}^{\text{cross}}(t)\right]^+$ 后，近端 DCI A 按如下控制律计算组 $p$ 的目标注入速率：

$$R_{A,p}^{\mathrm{cmd}}(t) = \operatorname{clip}\left( \hat{C}_{\mathrm{avail},p}(t) - \frac{\hat{Q}_{B,p}(t + d_{AB}) - Q_{B,p}^*}{T_{\mathrm{drain}}}, \; 0, \; \min\left(C_A, \, C_{B,p}\right) \right)$$

**为什么采用这一控制律？（理论依据与替换分析）**：
- **控制论本质**：该控制律由**前馈带宽匹配项（Feedforward Capacity Matching）** $\hat{C}_{\text{avail},p}(t)$ 与**预测队列比例消除项（Predictive Queue Proportional Drain）** $-\frac{\hat{Q}_{B,p}(t + d_{AB}) - Q_{B,p}^*}{T_{\text{drain}}}$ 组成，与 [XCP](https://www.google.com/search?q=https%3A%2F%2Fconferences.sigcomm.org%2Fsigcomm%2F2002%2Fpapers%2Fxcp.pdf) 的效率控制器（Efficiency Controller, $\Phi = \alpha \cdot d \cdot S - \beta \cdot Q$）及 [Mascolo Smith 预测控制器](https://www.google.com/search?q=http%3A%2F%2Fcongres.cran.univ-lorraine.fr%2F2001%2FECC_2001%2Farticles%2Fecc3931.pdf)一脉相承。
- **闭环动态证明**：当速率未饱和且 $Q_{B,p} > 0$ 时，远端真实队列在 $\tau = t + d_{AB}$ 时刻的变化率为：
  $$\dot{Q}_{B,p}(t + d_{AB}) = R_{A,p}^{\text{actual}}(t) - C_{\text{avail},p}(t + d_{AB})$$
  当 A 处有充足積压（$R_{A,p}^{\text{actual}}(t) = R_{A,p}^{\text{cmd}}(t)$）且预测误差有界（$\hat{Q}_{B,p}(t+d_{AB}) = Q_{B,p}(t+d_{AB}) + \epsilon_Q$，$\hat{C}_{\text{avail},p}(t) = C_{\text{avail},p}(t+d_{AB}) + \epsilon_C$）时，代入控制律得：
  $$\dot{Q}_{B,p}(\tau) + \frac{1}{T_{\text{drain}}}\left(Q_{B,p}(\tau) - Q_{B,p}^*\right) = \epsilon_C - \frac{\epsilon_Q}{T_{\text{drain}}}$$
  可见，前向重放预测器完全抵消了环路纯滞后 $e^{-s\cdot \text{RTT}_{AB}}$，使远端队列偏差 $(Q_{B,p} - Q_{B,p}^*)$ 以时间常数 $T_{\text{drain}}$ 呈**一阶无振荡指数衰减**！

**关键超参数整定准则与边界保护（解决草稿未定问题）**：
1. **排空时间常数 $T_{\text{drain}}$ 的鲁棒下界**：
   虽然理想 Smith 预测器允许任意小的 $T_{\text{drain}}$，但在实际网络中，远端交叉流量在 $[t, t+d_{AB}]$ 期间可能发生未知突变（导致模型失配误差 $\epsilon_Q, \epsilon_C \neq 0$），且该误差需经 $\text{RTT}_{AB}$ 才被下一次反馈观测校正。由不确定时滞系统的鲁棒稳定性条件，$T_{\text{drain}}$ 不可小于反馈采样周期的 2 倍及往返时延的鲁棒比例系数：
   $$T_{\text{drain}} \ge \max\left( 2\Delta_{\text{fb}}, \; \alpha_{\text{rob}} \cdot \text{RTT}_{AB} \right), \quad \alpha_{\text{rob}} \in [0.25, \, 0.5]$$
   相比无预测控制必须取 $T_{\text{drain}} \ge 1.5\sim 2\,\text{RTT}_{AB}$ 才能避免振荡，R5 将收敛速度提升了 $3\sim 6$ 倍。
2. **目标队列 $Q_{B,p}^*$ 的抗排空（Anti-Underflow）设定**：
   维持 $Q_{B,p}^* > 0$ 的目的是建立“小水库缓冲垫”，防止当远端交叉流量突然消失或上游主机瞬时供给波动时，在 A 新增流量经 $d_{AB}$ 到达 B 之前 B 的出口发生空转（Underflow）。$Q_{B,p}^*$ 应设为容忍单向时延内净服务率正向跳变 $\Delta C_{\text{max}}$ 所需的最小缓冲：
   $$Q_{B,p}^* = \beta_q \cdot C_{B,p} \cdot d_{AB} \quad (\text{典型值 } \beta_q \in [0.05, \, 0.15])$$
   这意味着远端稳态排队仅需 $0.05\sim 0.15$ 个单向 BDP（远低于传统 PFC 所需的 $2\,\text{BDP}$ 或 [SWING](https://www.google.com/search?q=https%3A%2F%2Fcs.nju.edu.cn%2Ftianchen%2Flunwen%2F2023%2Ftpds23yanqing.pdf)/[Bifrost](https://www.google.com/search?q=https%3A%2F%2Fcs.nju.edu.cn%2Ftianchen%2Flunwen%2F2024%2Fton24-chengyuan.pdf) 的 $1\,\text{BDP}$）。
3. **可选替代/增强形式（带抗饱和受限积分的 FF+PI 控制律与反馈超时衰减）**：
   - 若需要消除远端突发交叉流采样偏差引起的微小稳态静差，可在上述比例项后增加一个带抗积分饱和（Clamped Anti-Windup）的弱积分项 $-K_I \int (Q_{B,p}(s) - Q_{B,p}^*) ds$（仅在 $R_{A,p}^{\text{cmd}}$ 未触及 $0$ 或 $C_A$ 边界时积分）。建议论文将 **FF+P 作为主公式**（仅含 $Q_{B,p}^*$ 与 $T_{\text{drain}}$ 两个直观物理参数），将 FF+PI 作为讨论或鲁棒性分析扩展。
   - **反馈陈旧度衰减（Staleness Guard）**：若因反向控制帧丢失或远端静默，A 距离上次收到组 $p$ 反馈已超过 $\Delta t_{\text{age}} > K_{\text{stale}}\Delta_{\text{fb}}$，则对 $\hat{C}_{\text{avail},p}(t)$ 施加指数保守衰减或回退至安全基线速率，防止开环盲发。

#### 4.3.2 多端口容量约束与组内工作保守调度（Work-Conserving Hierarchical Shaper）
为了在不引入 R4 需求注册与流完成通知的前提下，同时实现“竞争同一远端出口的流共享上限”、“空闲或受限流不占死配额”以及“短流快速启动”，R5 在近端 DCI A 设计**两级分层调度整形器**：

1. **第一级（跨组长链路总容量约束）**：
   若 A 同时向 B 的多个出口端口 $p \in \mathcal{P}_B$ 发送流量，所有活跃组的目标速率之和不得超过长链路物理容量 $C_A$。若 $\sum_{p \in \mathcal{P}_B^{\text{act}}} R_{A,p}^{\text{cmd}}(t) > C_A$，则按各组目标速率比例（或 Max-Min 公平）归一化实际组整形速率 $\tilde{R}_{A,p}^{\text{cmd}}(t)$。
2. **第二级（组内单流工作保守调度：短流优先 + 长流 DRR）**：
   在每个组 $p$ 内部，仅设置**一个速率为 $\tilde{R}_{A,p}^{\text{cmd}}(t)$ 的组级令牌桶（Group Token Bucket Shaper）**，组内所有流 $i \in \mathcal{F}_p$ 共享该令牌桶，按以下两层工作保守规则出队：
   - **无先验短流快速通道（Size-Unaware Short-Flow Priority）**：A 维护每条活跃流的已发送字节计数器 $B_i$（流队列空闲超时后自动回收表项，无需显式流完成通知）。当 $B_i < \Theta_{\text{short}}$（例如 $128\,\text{KB}$）时，流 $i$ 处于“短流/新流态”，排入组 $p$ 的高优先级子队列；一旦 $B_i \ge \Theta_{\text{short}}$，自动降级至组 $p$ 的长流公平子队列。为防止极端短流洪泛饿死长流，高优子队列最多消耗组令牌的 $\gamma_{\text{short}}$（如 $80\%$）。
   - **长流赤字轮询（Deficit Round Robin, DRR）工作保守共享**：处于长流态且本地子队列非空的流 $i \in \mathcal{F}_{p}^{\text{long, active}}$，通过 DRR 轮询消耗组级令牌桶剩余令牌。
   - **冷启动安全突发**：若组 $p$ 此前完全空闲（无近期反馈或预测远端队列为 $0$），新流到达时赋予组整形器初始探测速率 $R_{A,p}^{\text{init}} = \min(C_A, C_{B,p})$ 及最大未确认突发配额 $B_{\text{init}} \le B_{B,p}^{\text{headroom}}/2$；待 1 个 $\text{RTT}_{AB}$ 后首个远端反馈到达，立即切入预测闭环。

> **机制优势**：
> - 当流 $j$ 因源端 RNIC 限速或应用层停顿导致其在 A 处的子队列为空时，DRR 调度器在当前轮次直接跳过流 $j$，将其未使用的组级整形容量**零延迟、100%地分配给同组其他有积压的流**；
> - 短流在组级安全预算 $\tilde{R}_{A,p}^{\text{cmd}}(t)$ 内获得近乎线速的抢占式转发，既避免了等待 1 个 $\text{RTT}_{AB}$ 反馈的启动延迟，又消除了无约束并发短流（Incast）击穿远端 B 缓存的风险。

### 4.4 机制三：基于水位与一阶导数双重门控的近端 CNP 主机反压

#### 4.4.1 为什么近端 CNP 必须与远端拥塞解耦？
在 R5 中，远端队列安全已由 A 的出境整形器 $\tilde{R}_{A,p}^{\text{cmd}}(t)$ 严格兜底。当 $\tilde{R}_{A,p}^{\text{cmd}}(t)$ 因远端拥塞而下调时，由于源端主机仍以原速率发送，差额流量 $(\lambda_{A,i}^{\text{in}} - \mu_{A,i}^{\text{out}})$ 会在近端 DCI A 的本地缓存中积压。此时近端 CNP 的唯一职责是：**约束源端 DCQCN 主机，使主机注入 A 的速率逐渐回落至与 A 的实际出队速率 $\mu_{A,i}^{\text{out}}$ 匹配，防止 A 本地缓存溢出或触发源端网络 PFC 风暴**。

如果 A 看到本地队列 $Q_{A,i}(t)$ 较高就无条件连续发送 CNP，由于历史积压的排空需要时间（即便主机已经把速率降到了 $\lambda_{A,i}^{\text{in}} < \mu_{A,i}^{\text{out}}$，队列 $Q_{A,i}$ 在排空过程中依然高于阈值），持续追加的 CNP 会迫使主机 RNIC 连续执行乘法降速，最终导致主机发送率远低于 $\mu_{A,i}^{\text{out}}$，造成 A 缓存排空断流与吞吐损失。

#### 4.4.2 导数门控近端 CNP 触发准则（Derivative-Gated Proxy CNP）
近端 DCI A 以源端微秒级周期 $\Delta_A$（如 $20\sim 50\,\mu\text{s}$）监测流 $i$（或其对应队列）的本地缓存占用 $Q_{A,i}(t)$、本地到达速率 $\lambda_{A,i}^{\text{in}}(t)$ 与本地实际出队速率 $\mu_{A,i}^{\text{out}}(t)$，计算本地队列净变化率（一阶导数）：
$$\dot{Q}_{A,i}(t) = \lambda_{A,i}^{\text{in}}(t) - \mu_{A,i}^{\text{out}}(t)$$

A 向流 $i$ 的源端 QP 生成并发送近端 CNP，**当且仅当同时满足以下三个条件**：
1. **水位越界或前瞻越界条件（Queue Level Condition）**：
   $$Q_{A,i}(t) + \max\left(0, \, \dot{Q}_{A,i}(t)\right) \cdot \text{RTT}_{\text{src}} \ge K_{A}^{\min}$$
   其中 $K_A^{\min}$ 为本地缓存反压起始水位，$\text{RTT}_{\text{src}}$ 为源端数据中心内部往返时延。
2. **非有效排空门控条件（Non-Draining Derivative Guard）**：
   $$\dot{Q}_{A,i}(t) > -\kappa_{\text{drain}} \cdot \frac{\left[Q_{A,i}(t) - K_{A}^{\text{target}}\right]^+}{\tau_{\text{local}}}$$
   其中 $K_A^{\text{target}} \le K_A^{\min}$ 为本地期望安全水位，$\tau_{\text{local}}$ 为期望本地排空时间尺度，$\kappa_{\text{drain}} \in (0, 1]$。
   - **直观物理意义**：一旦主机已经响应先前的 CNP 将发送速率降了下来，使得 $\lambda_{A,i}^{\text{in}}(t) < \mu_{A,i}^{\text{out}}(t)$ 且本地队列正在以不低于目标斜率的速度排空（$\dot{Q}_{A,i}(t) \le -\kappa_{\text{drain}}\frac{Q_{A,i}-K_A^{\text{target}}}{\tau_{\text{local}}}$），即使此时 $Q_{A,i}(t)$ 仍高于 $K_A^{\min}$，**A 也立即停止追加普通近端 CNP**！
   - **极限安全兜底（Hard Ceiling Override）**：仅当本地队列逼近物理缓存极限 $Q_{A,i}(t) \ge K_A^{\text{crit}}$（即将触发源端 PFC 阈值 $X_{\text{off}}$）且 $\lambda_{A,i}^{\text{in}}(t) > 0$ 时，才无视导数门控强制发送紧急 CNP。
3. **频率限幅条件（Pacing Timer Condition）**：
   距离上次向该 QP 发送 CNP 的时间间隔不少于 $\Delta_{\text{CNP}}^{\min}$（如 $50\,\mu\text{s}$，与商用 RNIC 的 DCQCN 最小降速间隔对齐）。

### 4.5 机制四：沿途 ECN 标记与远端原生 CNP 的空间解耦处理规则

针对草稿未决问题 4（“接收端返回的原生 CNP 与沿途 CE 标记怎么处理”），若不加区分地开启全路径 ECN 标记并透传所有原生 CNP，会引发严重的**控制环路冲突**。R5 确立清晰的**空间职责划分（Spatial Partitioning）规则**：

1. **远端 DCI B 的受控出口队列（由 A-B 预测环接管）——关闭低阈值 ECN 标记**：
   由于 R5 控制律有意在远端 B 的出口维持一个非零的小目标队列 $Q_{B,p}^* > 0$ 以保证长链路 100% 利用率，若 B 的出口队列仍按常规 DCQCN 低阈值打 CE 标记，则所有经过 B 的数据包都会被标记并触发接收端返回原生 CNP。这会导致源端主机在 1 个 $\text{RTT}_{\text{WAN}}$ 后大幅降速，掏空 A 的缓存并破坏预测整形（该现象已在 [LSCC](https://www.google.com/search?q=http%3A%2F%2Fstaff.ustc.edu.cn%2F%7Ekpxue%2Fpaper%2FIWQoS24-MinfeiLong.pdf) 中被严格证实）。因此，在 B 被 A 预测控制的队列上，**应禁用常规 ECN 标记**（或仅将 ECN 阈值设为远高于 $Q_{B,p}^*$ 的异常保护上界）。
2. **目的端域内网络交换机（B 下游至接收主机）——保留原生 ECN 标记与原生 CNP 透传**：
   当拥塞发生在 B 下游的目的端 ToR/Spine 交换机（例如目的端域内流量与跨域流量在最后一跳汇聚产生 Incast），且尚未通过 PFC 反压到 B 的出口端口时，B 的出口队列长度 $Q_{B,p}$ 无法反映该下游拥塞。此时保留目的端域内交换机的 ECN 标记，并让接收主机生成的**原生 CNP 透传通过 B 和 A 回到发送主机**（A 亦可旁路观测该原生 CNP 作为对应流/组的辅助降速信号），即可在不实现 B 局部限速的当前阶段，为目的端域内拥塞提供兜底保护。
3. **源端域内网络交换机（发送主机至 A 上游）——A 代发近端 CNP 并清除 CE 位**：
   若数据包在进入 A 之前的源端交换机已被打上 CE 标记，说明源端域内链路发生拥塞。若任由该 CE 标记跨越长链路去接收端再绕回，反馈延迟高达 $\text{RTT}_{\text{WAN}}$。因此，A 在入向检测到数据包携带 `ECN == CE (11)` 时，应立即触发近端 CNP 返回给发送主机以在 $\text{RTT}_{\text{src}}$ 内快速收敛源端拥塞，同时**将该出境数据包头的 CE 位重置为 `ECT (10)`**（借鉴 [THEMIS](https://www.google.com/search?q=https%3A%2F%2Fzhangmenghao.github.io%2Fpapers%2FICNP2025-Themis.pdf)、[GRC](https://www.google.com/search?q=https%3A%2F%2Fzhangmenghao.github.io%2Fpapers%2FICNP2026-GRC.pdf) 与 [Spillway](https://www.google.com/search?q=https%3A%2F%2Farxiv.org%2Fhtml%2F2605.11852v2)），杜绝接收端在 1 个 $\text{RTT}_{\text{WAN}}$ 后因同一源端拥塞再次产生重复的原生 CNP。

## 5. 相关工作核实、借鉴与实质差异对比

围绕 R5 的四个核心设计要素（跨时延状态预测、显式队列排空控制律、DCI 分段拥塞控制、无损流控预测），我们核实了相关领域的代表性原始文献，明确 R5 的理论借鉴来源与实质性差异（绝不将已有控制论结构包装为凭空发明，而是准确界定 R5 在跨域 RDMA 网关整形与双环解耦上的独特贡献）：

| 相关工作与出处 | 核心机制与适用前提 | R5 借鉴的合理内核 | R5 与该工作的实质性差异（为什么不能直接照搬） |
| :--- | :--- | :--- | :--- |
| **[Mascolo Smith 预测器](https://www.google.com/search?q=http%3A%2F%2Fcongres.cran.univ-lorraine.fr%2F2001%2FECC_2001%2Farticles%2Fecc3931.pdf)** (Automatica 1999 / ECC 2001) & **[RSP](https://www.google.com/search?q=https%3A%2F%2Fwww.researchgate.net%2Fpublication%2F224165926_RSP_Robust_smith_predictor_for_queue_management_in_time-delay_networks)** | 针对 ATM ABR / 广域网大时延队列管理，利用 Smith 原理在控制器端加总过去 1 个 RTT 的在途发送量来补偿死区时间（Dead Time）。适用前提：线性队列模型或已知固定服务率。 | 借鉴其**用前向状态预测抵消闭环纯滞后 $\text{RTT}_{AB}$** 的控制论基本原理，使比例控制器在长时延下保持一阶无振荡稳定。 | 传统 Smith 预测器采用连续/离散**线性积分**（即在整个 RTT 窗口上直接将总到达减总服务），忽略了队列在中间时刻先排空至 $0$（$[x]^+$ 非线性边界）再重新积压的物理过程，且未区分“配置速率”与“因上游受限导致的真实出队量”。R5 提出了**基于锚点回显的无时钟同步分段非线性重放（Piecewise Non-Linear Replay with真實出队记账）**。 |
| **[XCP](https://www.google.com/search?q=https%3A%2F%2Fconferences.sigcomm.org%2Fsigcomm%2F2002%2Fpapers%2Fxcp.pdf)** (Katabi et al., SIGCOMM 2002) & **[HPCC](https://www.google.com/search?q=https%3A%2F%2Frmiao.github.io%2Fassets%2Fpdf%2Fhpcc-li.pdf)** (Li et al., SIGCOMM 2019) | 路由器/交换机基于剩余带宽与队列排空项（$\alpha d S - \beta Q$）或带内遥测（INT）计算精确窗口/速率。适用前提：需要定制端主机协议栈与沿途所有交换机支持专用报文头。 | 借鉴其**“可用容量前馈匹配 + 队列偏差按目标时间常数 $T_{\text{drain}}$ 排空”**的显式速率控制律结构。 | XCP 和 HPCC 是端到端协议，要求修改端主机网卡/协议栈，且在跨 DC 场景下端到端反馈仍受制于长距 RTT 延迟（[LSCC](https://www.google.com/search?q=http%3A%2F%2Fstaff.ustc.edu.cn%2F%7Ekpxue%2Fpaper%2FIWQoS24-MinfeiLong.pdf) 实验表明原生 HPCC 在长链路上仍会出现启动期剧烈队列堆积）。R5 将该控制律部署在**近端 DCI 网关 A 的预测状态 $\hat{Q}_{B,p}(t+d_{AB})$ 上**，对商用 DCQCN 网卡完全透明。 |
| **[LSCC](https://www.google.com/search?q=http%3A%2F%2Fstaff.ustc.edu.cn%2F%7Ekpxue%2Fpaper%2FIWQoS24-MinfeiLong.pdf)** (Long et al., IWQoS 2024) | 在两端边界交换机间建立分段反馈环：远端维护长度为 $N=2D/T$ 的动态反馈列表扣除在途未生效调整量 $R_a(t-jT)$；近端维护**伪队列** $\Delta Q_e^*=\int(R_{\text{in}}-R)dt$ 向源端发 Direct CNP。 | 借鉴其**长距链路分段闭环**思想，以及**远端受控队列必须关闭低阈值 ECN 标记**以避免双重反馈振荡的结论。 | **这是与 R5 最接近但路线截然相反的工作**：<br>① **谁执行预测**：LSCC 在远端 B 侧预测，且假设近端 A 能“立即”将主机速率调整为反馈率 $R$，若主机未及时收敛则预测失准；R5 在**近端 A 侧基于真实已出队历史 $X_{A,p}$ 预测**，天然免疫上游供给不足与速率未收敛误差。<br>② **谁执行限速**：LSCC 不在 A 处直接做流量整形，而是靠伪队列驱动近端 CNP 逼迫主机降速（把远端拥塞与近端 CNP 强耦合）；R5 **在 A 处直接执行精确组级整形**，将近端 CNP 解耦为纯粹的本地缓存一阶导数门控保护，消除了 DCQCN 锯齿振荡向长链路的传导。 |
| **[GRC](https://www.google.com/search?q=https%3A%2F%2Fzhangmenghao.github.io%2Fpapers%2FICNP2026-GRC.pdf)** (Zhang et al., ICNP 2026) & **[BiCC](https://www.google.com/search?q=https%3A%2F%2Fieeexplore.ieee.org%2Fdocument%2F10621412%2F)** (Wan et al., INFOCOM 2024) | GRC 在近端网关监控 WAN 时延并计算 `bytesDiff = realBytes - refBytes`，按 RED 概率向主机发 CNP；BiCC 利用双侧 DCI 交换机与接收侧 VOQ 分段生成 CNP。 | 借鉴其**对商用 RNIC 零修改**的网关代理部署模式，以及拦截源端 CE 标记生成近端 CNP 后清除 CE 位的防重复机制（[THEMIS](https://www.google.com/search?q=https%3A%2F%2Fzhangmenghao.github.io%2Fpapers%2FICNP2025-Themis.pdf)）。 | GRC 与 BiCC 依然依赖**向端主机注入 CNP 来间接控制进入 WAN 的流量**（GRC 甚至发现跨周期累积带符号字节差会导致主机速率无法收敛）。R5 则通过**近端网关硬件/软件整形器直接切断长链路超发**，主机侧仅需维持粗粒度非空供给即可跑满长链路。 |
| **[Bifrost](https://www.google.com/search?q=https%3A%2F%2Fcs.nju.edu.cn%2Ftianchen%2Flunwen%2F2024%2Fton24-chengyuan.pdf)** (ICNP 2023 / ToN 2024) & **[SWING](https://www.google.com/search?q=https%3A%2F%2Fcs.nju.edu.cn%2Ftianchen%2Flunwen%2F2023%2Ftpds23yanqing.pdf)** (TPDS 2023) | 面向长距无损链路的 L2 流控改进：SWING 用中继器做周期性 PFC-Relay；Bifrost 在下游根据历史 Pause 记录维护在途包上界（Virtual Incoming Packets）提前发送精细 Pause 帧。 | 借鉴其**显式核算在途流量（In-Flight Bytes）以将远端缓存需求压缩至 1 BDP 以内**的分析视角。 | SWING 和 Bifrost 本质上仍是**链路层暂停机制（Pause-based L2 Flow Control）**，通过高频率发送 Pause 帧（如每 $10\,\mu\text{s}$ 一帧）暂停整个物理端口或优先级队列，容易造成跨端口队头阻塞（HoL Blocking）且无法感知多流公平性。R5 是**按远端出口竞争组组织的细粒度速率整形与流级 DRR 调度**，不暂停长链路物理端口。 |

## 6. 适用边界、代价限制与失效条件分析

任何系统设计都有其明确的能力边界。诚实界定 R5 的代价、约束与潜在失效条件，不仅能增强论文论证的严密性，也直接指导验证实验的设计：

1. **近端 DCI A 的缓存与队列管理资源代价（Resource Overhead at Near-End DCI A）**：
   - **代价转移原理**：R5 将长距链路高 BDP 的缓冲压力从远端 DCI B 转移并转化为“近端 DCI A 的整形缓存 + 源端内网微秒级近端 CNP 反压”。
   - **所需缓存下界**：由于近端 CNP 从 A 发往源端主机并使 RNIC 降速的闭环时延仅为源端域内往返时延 $\text{RTT}_{\text{src}}$（通常 $< 20\,\mu\text{s}$，远小于跨 DC 的 $\text{RTT}_{AB} \sim \text{ms}$），A 处为吸收主机响应滞后所需的理论最小Headroom 仅为 $O(C_A \cdot \text{RTT}_{\text{src}} + \Delta R \cdot \tau_{\text{DCQCN}})$（百 KB 至数 MB 量级），而非长链路的 $\text{BDP}_{AB}$（数十至上百 MB）。但如果源端主机数量极大（大规模 Incast 汇入 A）且主机 DCQCN 降速步长过慢，A 的缓存仍可能承压，此时需依赖源端内网的原生 PFC 作为最后兜底。
2. **远端交叉流量高频剧烈振荡下的预测失配边界（High-Frequency Cross-Traffic Volatility）**：
   - R5 的前向重放假设在未来一个单向时延 $d_{AB}$（或采样间隔）内，远端端口 $p$ 的净服务能力 $\hat{\mu}_{B,p}^{\text{net}} = \hat{\mu}_{B,p}^{\text{out}} - \hat{\lambda}_{B,p}^{\text{cross}}$ 可由近期测量外推。
   - **失效条件**：当远端端口 $p$ 遭遇**周期恰好与 $\text{RTT}_{AB}$ 同量级且幅度接近 $100\%\,C_{B,p}$ 的方波式域内突发交叉流量（如目的端本地大规模 All-to-All 集合通信微突发，见 [Spillway](https://www.google.com/search?q=https%3A%2F%2Farxiv.org%2Fhtml%2F2605.11852v2)）**时，由于任何位于 A 处的控制器都受制于光速传播极限 $d_{AB}$，A 在时刻 $t$ 发出的流量无法预知 $[t, t+d_{AB}]$ 期间才在目的端新诞生的本地突发。此时远端 B 必须依靠约 $\Delta C_{\text{burst}} \cdot \min(T_{\text{burst}}, \text{RTT}_{AB})$ 的物理缓存裕度来吸收这一不可预测分量。
3. **长距链路存在高随机丢包或动态多路径乱序（Lossy or Multipath WAN）**：
   - 基础版前向重放假设 A 到 B 为保序、近无损的专用长距链路（或固定隧道），使得 $[\tau_k, \tau_{k+1}]$ 发出的字节按相同时间间隔到达 B。若广域网存在严重的多路径时延差（ECMP Reordering）或高随机丢包，时域重放窗口会发生扩散畸变，需结合链路层重传或序列号区间对齐。
4. **目的端深度域内拥塞（Downstream Multi-Hop Bottleneck beyond B）**：
   - 当前版本暂未开启远端 B 的局部下游限速。若瓶颈位于目的端 DCN 内部（如目的端 ToR 下联口），且目的端未通过 PFC 将反压传回 B 的出口端口，则 A-B 预测环观测到的 $Q_{B,p}$ 保持低位，此时退化为依赖透传的目的端原生 CNP 进行端到端降速。

## 7. 核心研究主张、验证思路与证伪判据（Falsification Criteria）

为确保论文实验设计的科学性，我们将 R5 的设计转化为 **4 条可独立检验的核心研究主张（Claims）**，并明确界定每条主张的验证方法及**什么实验结果会削弱或否定该主张**。

| 核心研究主张（Claim） | 验证实验设计（不涉具体代码实现） | 支撑主张的预期指标 | 削弱或否定该主张的证伪判据（Falsification Criteria） |
| :--- | :--- | :--- | :--- |
| **主张 1（预测重放的有效性）**：相比直接使用滞后反馈队列 $Q_B(s)$ 或线性总体截零估计，基于真实出队历史的分段非线性前向重放能显著降低 $\hat{Q}_B(t+d_{AB})$ 的预测误差，消除远端队列的周期性超调与排空。 | **消融对比（Ablation）**：固定控制律与近端 CNP，对比四种状态输入：<br>① 滞后反馈 $Q_B(s)$（无预测）；<br>② 按目标速率 $R^{\text{cmd}}$ 重放；<br>③ 按真实出队 $X_A$ 整窗求和再截零；<br>④ R5 分段微步长 $[x]^+$ 重放。<br>在长链路 $\text{RTT}_{AB} \in [0.5, 20]\,\text{ms}$ 及动态启停流下测试。 | ④ 的预测队列与 $t+d_{AB}$ 时刻真实到达后的 $Q_B$ 均方根误差（RMSE）最小；远端队列紧密收敛在 $Q_B^*$ 附近，长链路利用率 $>95\%$ 且零 PFC 触发。 | - 若④与①/③在远端队列峰值、99 分位排队时延及吞吐率上无显著差异（差距 $<5\%$）；<br>- 或当存在间隙流量（先排空后突发）时，③与④表现一致，则说明分段非线性重放的复杂度是不必要的。 |
| **主张 2（整形与近端 CNP 解耦的优越性）**：由近端 DCI A 直接整形控制远端队列、近端 CNP 仅按“水位 + 导数门控”保护本地缓存，优于仅靠代理 CNP 驱动主机降速（如 LSCC/GRC 类方案）或对同一远端拥塞同时整形并发 CNP 的强耦合方案。 | **机制对比实验**：在相同远端带宽阶跃下降/恢复场景下，对比：<br>① 纯代理 CNP 无 A 整形（LSCC/GRC 模式）；<br>② A 整形 + 远端拥塞直接触发近端 CNP（双重控制）；<br>③ A 整形 + 仅基于本地队列阈值的近端 CNP（无导数门控）；<br>④ R5（A 整形 + 导数门控近端 CNP）。 | ④ 在远端可用带宽恢复瞬间，A 缓存不发生断流（Underflow），长链路吞吐恢复时间最短，且发送端 RNIC 速率波动方差最小、近端 CNP 报文开销最低。 | - 若①（纯代理 CNP）在商用 DCQCN 参数下即可实现与④相当的远端队列控制精度与带宽利用率，则否定“必须在 A 处直接整形”的前提；<br>- 若②或③不会引起 A 队列排空断流与吞吐跌落，则削弱“双重控制危害”与“导数门控必要性”的主张。 |
| **主张 3（无需求注册的分层工作保守与短流快速通道）**：无需 R4 的显式需求注册与完成通知，组级令牌桶配合组内“短流字节阈值优先 + 长流 DRR”即可同时实现慢供给流配额回收与短流低 FCT，且不会在并发短流下冲垮远端缓存。 | **混合负载与受限流测试**：<br>① 构造同组 2 条长流，人为限制其中 1 条流在源端 ToR 的可用带宽，检验另 1 条流能否立即吃满组配额 $\tilde{R}_{A,p}^{\text{cmd}}$；<br>② 在 WebSearch / Ali-WAN 真实长短流混合负载及并发短流 Incast 下，对比静态均分、无约束短流线速启动与 R5 分层调度。 | ① 未受限流在数微秒内无缝接管剩余带宽，组总出队率等于 $\tilde{R}_{A,p}^{\text{cmd}}$；<br>② 相比无约束短流线速启动，R5 在高并发短流下保持远端零丢包/零 PFC，同时短流 99 分位 FCT 显著低于无短流优先方案。 | - 若当部分流在源端受限时，组总实际吞吐显著低于 $\tilde{R}_{A,p}^{\text{cmd}}$；<br>- 或在组预算内优先调度短流导致短流 FCT 相比 R4（带需求注册）显著恶化，则削弱去协议化工作保守设计的主张。 |
| **主张 4（远端出口不自限速原则）**：当瓶颈位于远端 DCI B 自身出口端口时，仅由近端 A 预测整形而不对 B 出口限速，相比在 B 出口施加限速具有更高的有效吞吐与更短的排空时间。 | **对照实验**：在 B 出口端口拥塞场景下，开启/关闭 B 出口侧附加限速器，测量长链路有效吞吐（Goodput）、B 队列排空时间与 FCT。 | 关闭 B 出口自限速时，端口始终维持 100% 物理线速排空，平均与尾部 FCT 更低。 | 若在 B 出口自身拥塞时叠加 B 本地限速反而降低了 FCT 或未造成吞吐损失，则否定该主张。 |

## 8. 候选论文贡献总结（Candidate Contributions）

基于上述完善后的设计体系与文献对标，本工作可提炼出以下 **3 项逻辑严密、具备明确学术区分度的核心贡献**：

1. **揭示跨数据中心 RDMA 网关代理控制中的“双重控制”与“在途非线性排空失真”机理**：
   从控制论与系统实证角度剖析了现有分段拥塞控制（依赖伪队列代发 CNP 间接限速或线性估算在途量）在长时延高 BDP 场景下的两个根本缺陷——即网关与商用 DCQCN 网卡的响应失配导致双重降速断流，以及忽略实际出队饱和与中间排空边界（$[x]^+$）导致的远端队列预测漂移。
2. **提出免时钟同步的分段前向重放预测器与分层工作保守整形架构（R5 核心主环）**：
   设计了由远端 DCI B 回显发送时间戳锚点与净服务能力、近端 DCI A 基于真实出队历史按微步长分段重放的非线性状态预测器，将跨 DC 滞后系统转化为一阶无振荡指数收敛系统；并结合组级预测整形器与组内“无先验短流优先 + 长流 DRR”两级工作保守调度，在无需任何需求注册协议与网卡修改的前提下，实现了远端浅缓存稳定控制（$<0.15\,\text{BDP}$）与高链路利用率。
3. **设计基于水位与一阶导数双重门控的近端 CNP 反压及 ECN 空间解耦机制（R5 协同次环）**：
   彻底解耦“远端队列精确整形”与“本地缓存反压保护”，提出利用本地队列净变化率（$\dot{Q}_A$）门控近端 CNP 生成的阻尼反压算法，并在空间上划分源端、远端受控队列与目的端域内交换机的 ECN/CNP 职责边界，消除了跨周期过度惩罚与多环路冲突。
