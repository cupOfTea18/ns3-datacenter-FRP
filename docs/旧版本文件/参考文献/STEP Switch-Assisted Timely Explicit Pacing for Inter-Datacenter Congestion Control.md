# STEP: Switch-Assisted Timely Explicit Pacing for Inter-Datacenter Congestion Control

**1st Given Name Surname** — dept. name of organization, name of organization, City, Country, email address or ORCID  
**2nd Given Name Surname** — dept. name of organization, name of organization, City, Country, email address or ORCID  
**3rd Given Name Surname** — dept. name of organization, name of organization, City, Country, email address or ORCID

---

## Abstract

Inter-DC communication leads to the co-existence of cross-DC flows and intra-DC flows. Constituting a latency discrepancy of 2 to 3 orders of magnitude between these two types of traffic, cross-DC flows suffer from a severe lag in responding to control signals. Although state-of-the-art control mechanisms can accelerate their response, alleviating the immediate impact inherently relies on the rapid rate adjustment of intra-DC flows.

We propose **STEP**, a fair-rate-driven explicit rate control scheme. In STEP, switches periodically compute fair rates based on queue lengths and virtual bytes, and feed them back to source nodes. When congestion occurs, agile intra-DC flows proactively yield bandwidth, while delayed cross-DC flows transmit at the assigned fair rate. Specifically, intra-DC flows employ aggressive backoff to quickly drain the queue, together with smooth rate pacing to ensure queue convergence. Testbed experiments and simulations show that STEP reduces flow completion time (FCT) by XX% compared with DCQCN. Moreover, STEP remains robust under long inter-DC propagation delays and guarantees fair bandwidth sharing among mixed traffic in steady state.

**Index Terms** — RDMA, Inter-datecenter Transmission, Explicit Rate Cotrol, Programmable Switches

---

## I. Introduction

The explosive growth of cloud computing, artificial intelligence, and big data services has led to an exponential expansion of data. Despite the emergence of hyper-scale datacenters [1], it remains challenging for the computational resources of a single datacenter to meet the demands of massive data processing in terms of scalability, fault tolerance, and cost-effectiveness [2]. Consequently, collaborative computing across geographically distributed datacenters has become an inevitable trend. Large-scale cloud service providers, such as Amazon Web Services (AWS), Microsoft Azure, and Google Cloud Platform (GCP), have achieved efficient inter-datacenter connectivity by deploying dedicated optical fibers (e.g., native 400 Gbps connections provided by AWS [3]) and utilizing the high-performance Remote Direct Memory Access (RDMA) protocol (where around 70% of traffic in Microsoft Azure is currently RDMA [4]).

Unfortunately, existing congestion control schemes within datacenters are insufficient to cope with the mixture of these two types of traffic. Being oblivious to traffic categories, the receiver blindly sends congestion notifications to the sources, expecting them to share the bandwidth fairly. As the latency of cross-DC flows is on the millisecond scale while that of intra-DC flows is on the microsecond scale [5], these flows fail to adjust their rates synchronously. Consequently, cross-DC flows cannot reduce their rates in time, leading to severe queue buildup. More severely, the shallow buffers inside datacenters are extremely limited compared to the massive volume of in-flight packets during long-distance transmission. Consequently, the continuous injection of cross-DC flows can easily trigger PFC pauses [6–8] or even buffer overflows. Due to the limited SRAM of commercial RDMA NICs (RNICs), RDMA packet loss recovery relies on a primitive Go-Back-N (GBN) retransmission mechanism, wherein the massive retransmission latency can incur a catastrophic transmission performance collapse [9].

Existing inter-datacenter congestion control schemes mitigate the impact of mixed traffic mainly by accelerating the response of cross-DC flows. For instance, ATC [10] and Torrent [11] partition long-distance data transmission into three independent control loops through two datacenter interconnection (DCI) switches, thereby improving performance over baseline schemes by X%. Furthermore, compared to implicit notifications, feeding back explicit and precise rate information enables ultra-fast convergence. Along this line, Bifrost [12] achieves port-level rate control by periodically transmitting control signals embedded with precise pause durations to upstream switches, while LSCC [13] enforces flow-level rate control by implementing rate calculation and limiting modules directly on the DCI (Datacenter Interconnect) switches. However, by focusing only on rate control for cross-DC flows, the aforementioned schemes enable efficient utilization of long-haul links but fail to ensure fair bandwidth allocation among different types of traffic.

Based on the above analysis, differentiated rate adjustment, switch feedback, and explicit rate control can effectively improve the transmission performance of mixed traffic. The roles of these three components and the objectives that the final system is expected to achieve are shown in Fig. 1. To this end, we design a fair-rate-driven explicit feedback strategy that allows the switch to periodically compute the fair rate and feed it back to the source nodes. Upon congestion caused by mixed traffic contention, intra-DC flows execute a backoff strategy to achieve rapid queue convergence, while cross-DC flows transmit at the designated fair rate.

| Component | Differentiated Control | Explicit Rate | Switch Feedback |
|---|---|---|---|
| **Function** | Fast Queue Convergence | Fewer Iterations | Short Feedback Loop |
| **Objective** | Lower FCT | High Utilization | Fairness |

*Fig. 1: Components, Functions and Objectives of STEP.*

We summarize the contributions of this paper as follows:

- STEP introduces an explicit rate control scheme for long-distance RDMA. Due to differences in feedback latency, inter-DC data transmission becomes a time-delay system. When the proportion of cross-DC flows is high, the system experiences severe oscillations and fails to reach a stable operating point. STEP computes the actual fair rate at the switch and uses it to guide data transmission.
- STEP achieves fast convergence through a differentiated rate control strategy. When congestion occurs, intra-DC flows perform aggressive backoff to rapidly drain the queue, while delayed cross-DC flows transmit directly based on the received fair rate.
- We implement STEP on commodity P4-based switches and conduct evaluations using both real-world testbed experiments and NS3 simulations. The extensive evaluation results show that STEP is a promising substrate for strengthening the performance of cross-datacenter RDMA networks.

Real-network testbed results and ns-3 simulation results in Section V show that, compared with DCQCN, STEP reduces FCT by %, improves effective bandwidth utilization by %, eliminates PFC triggering, and maintains high steady-state fairness.

---

## II. Background and Motivation

### A. Inter-Datacenter Transmission

Modern cloud services commonly span multiple geographically distributed datacenters, which are interconnected through private WANs built over high-capacity optical links, such as Google B4 [14], Microsoft SWAN [15], and Meta's Express Backbone [16]. In a typical deployment, DCI switches connect the local datacenter fabric to the long-haul WAN, as illustrated in Figure 2. Production measurements from Baidu's DCNs show that about 20% of high-priority traffic leaving a cluster traverses the WAN, and only 8.5% of DC pairs contribute 80% of such traffic. Moreover, WAN and intra-DC traffic increments are strongly correlated, with a correlation coefficient above 0.65, indicating that inter-DC traffic follows similar demand trends but changes more smoothly in magnitude [17].

*Fig. 2: Inter-datacenter connectivity via DCI switches.*

### B. Challenges of Mixed Traffic

Inter-datacenter transmission naturally mixes intra-DC and cross-DC traffic at DCI switches: intra-DC flows usually have microsecond-scale RTTs, whereas cross-DC flows experience millisecond-scale RTTs. When these flows share the same bottleneck, their different feedback time scales can lead to slow convergence, queue oscillations, and unfair bandwidth sharing. To understand the impact of such mixed traffic, we evaluate representative congestion control schemes, including DCQCN, TIMELY, HPCC, and RoCC, under different proportions of cross-DC flows while keeping the total number of flows fixed. Figure 3 illustrates how the proportion of cross-DC flows affects flow completion time (FCT).

*Fig. 3: Fake image as placeholder.*

### C. Motivation

In existing congestion control schemes, all flows are marked and regulated under the same mechanism. For simplicity, we assume that $F(t)$ denotes the desired transmission rate of each flow, while $N_1$ intra-DC flows and $N_2$ cross-DC flows compete for a bottleneck link with capacity $c_l$. Let $T_1$ and $T_2$ denote their respective response delays. Then, we have:

$$
R_{\text{intra}}(t) = F(t - T_1) \\
R_{\text{cross}}(t) = F(t - T_2) \tag{1}
$$

$$
\frac{dQ(t)}{dt} = \frac{\Delta F}{\Delta Q} \left[ N_1 R_{\text{intra}}(t) + N_2 R_{\text{cross}}(t) \right]
$$

By applying the Laplace transform, the discrete time-domain model is transformed into the continuous frequency domain, yielding the frequency-domain representation of the queue length $Q(s)$. The correspondence between the frequency domain and real-world scenarios is summarized in Table I.

$$
Q(s) = \frac{\kappa}{s} \left[ N_1 e^{-sT_1} R_{\text{intra}}(s) + N_2 e^{-sT_2} R_{\text{cross}}(s) \right] \tag{2}
$$

**TABLE I: Frequency-to-phenomenon mapping**

| Frequency | Phenomenon |
|---|---|
| Low-frequency | Long-term congestion trend |
| Mid-frequency | Intra-DC RTT-level feedback oscillations |
| High-frequency | Bursty traffic / queue jitter |

In the above formulation, $\kappa = \frac{\Delta Q}{\Delta F}$ denotes the precision tuning parameter. We define the error signal as $E(s) = Q_{\text{ref}} - Q(s)$. Since the feedback rate depends on the queue deviation, we model it as $F(s) = c(s)E(s)$. Therefore, the open-loop transfer function expressed as $G(s) = \frac{Q(s)}{E(s)}$, and the system control block diagram is illustrated in Fig. 4.

*Fig. 4: System control block diagram.*

$$
G(s) = \frac{\kappa}{s} c(s) \left[ N_1 e^{-sT_1} + N_2 e^{-sT_2} \right] \tag{3}
$$

In the open-loop transfer function, the first term captures the effect of intra-DC flows, while the second term represents cross-DC flows. Since $T_2$ is two to three orders of magnitude larger than $T_1$, it introduces a significant propagation delay into the system. As a result, in the low-frequency regime, intra-DC and cross-DC flows exert comparable influence on the system dynamics. However, in the mid- and high-frequency regimes, the contribution of intra-DC flows becomes significantly more dominant than that of cross-DC flows, which is consistent with practical observations.

*Fig. 5: Impact of Cross-DC Flow Ratio on System Stability.*

**Stability Analysis:** Figure 5 shows two effects of increasing the fraction of cross-DC flows. First, the gain crossover frequency $\omega_{gc}$ shifts to a lower frequency as cross-DC traffic is introduced. For example, $\omega_{gc}$ decreases from about $4.0 \times 10^3$ rad/s when $(N_1 = 10, N_2 = 0)$ to about $2.4 \times 10^3$ rad/s when $(N_1 = 5, N_2 = 5)$. This indicates a reduced closed-loop bandwidth, leading to slower queue response and longer convergence time.

Second, cross-DC flows significantly reduce the stability margin. At the gain crossover frequency, the phase is close to $-180^\circ$ when intra-DC flows dominate, but drops to around $-250^\circ$ when $(N_1 = 5, N_2 = 5)$ and falls below the plotted range when $(N_1 = 2, N_2 = 8)$. Thus, as the fraction of cross-DC flows increases, the already limited phase margin further decreases, indicating weaker stability and a higher tendency toward queue oscillations.

**Design Insight:** In the current formulation, both intra-DC and cross-DC flows share the same transfer function $c(s)$, which limits the ability of intra-DC flows to effectively regulate convergence. To improve stability, the control gain associated with intra-DC flows should be strengthened, thereby mitigating the destabilizing effect introduced by delayed cross-DC feedback.

---

## III. Design

### A. Overview of STEP

STEP is an explicit rate control scheme for inter-datacenter RDMA, as shown in Fig. 6. It relies on switches to periodically generate Fair Rate Packets (FRPs) to provide fast and accurate rate feedback. Upon receiving FRPs, senders apply differentiated control: cross-DC flows directly transmit at the FRP-carried fair rate for rapid convergence, while intra-DC flows use backoff to quickly drain queues and maintain stability. This design achieves fast convergence, stable queues, and efficient resource utilization under mixed traffic. For clarity, the symbols used throughout this section are summarized in Table II.

*Fig. 6: The overview of STEP framework.*

**TABLE II: Symbols and definitions**

| Symbols | Definitions |
|---|---|
| $T$ | Generation period of FRP feedback packets |
| $T_1, T_2$ | Feedback delay of intra- and cross-DC flows |
| $Q_{\text{cur}}$ | Current queue length |
| $Q_{\text{old}}$ | Previous queue length |
| $Q_{\text{dev}}$ | Queue length deviation from $Q_{\text{ref}}$ |
| $Q_{\text{ref}}$ | Reference threshold of queue length |
| $Q_{\text{th}}$ | Deviation threshold of queue size |
| $F$ | Fair rate |
| $F_{\text{old}}$ | Previous fair rate |
| $F_{\text{min}}, F_{\text{max}}$ | Lower and upper bounds of fair rate |
| CP | Congested node |
| $N$ | Number of flows passing through a CP |
| $N_1, N_2$ | Number of intra-DC and cross-DC flows |
| $N_{\text{max}}$ | Maximum number of concurrent flows |
| $\alpha$ | Fair rate adjustment parameter |
| $\gamma$ | Drastic backoff parameter for intra-DC flows |
| $\Delta R$ | Timeout rate acceleration for cross-DC flows |
| $\text{backoff}$ | Backoff bytes of intra-DC flows |
| $v$ | Virtual bytes |

### B. Sender-side data transmission

**Differentiated rate control strategy.** Since cross-DC flows typically have millisecond-scale RTTs, whereas intra-DC flows have microsecond-scale RTTs, the sender can distinguish these two types of traffic based on their RTTs. In this work, we classify flows with RTTs greater than 1 ms as cross-DC flows. Upon receiving FRPs from switches, the sender enforces differentiated rate control for intra-DC and cross-DC flows (see Eq. 4). The detailed procedure is presented in Algorithm 1.

$$
R_{\text{intra}} = F - \gamma \times (Q_{\text{cur}} - Q_{\text{ref}}) \\
R_{\text{cross}} = F \tag{4}
$$

$F$ denotes the FRP-carried fair rate, and $\gamma$ represents the backoff coefficient of intra-DC flows. Due to the large delay in cross-DC feedback, queue accumulation induced by bursty traffic must be absorbed primarily through intra-DC rate reduction. Based on the open-loop transfer function derived in the previous section, we derive a stability-aware upper bound for $\gamma$ that enables fast queue convergence, with the detailed derivation provided in Section III-D:

$$
\gamma = \frac{\text{ScaleFactor}}{N_1} \tag{5}
$$

ScaleFactor is a tuning parameter, and $N_1$ denotes the number of intra-DC flows passing through the control point (CP). Eq. 5 indicates that $\gamma$ depends only on the number of intra-DC flows. However, in high-speed programmable switches, maintaining and tracking the exact number of active flows $N_1$ in real time incurs significant state and thread overhead.

To address this issue, STEP introduces an adaptive parameter $N$ at the sender side to approximate $N_1$ (Line 7). Specifically, $N$ is computed as the ratio between the current link bandwidth and the fair rate (Line 22), and is further constrained by a lower bound (Line 23) and an upper bound (Line 24). Since $N$ is strongly positively correlated with the actual number of active intra-DC flows, experimental results demonstrate that this lightweight approximation achieves high control accuracy with minimal hardware overhead.

**Algorithm 1 Differentiated Rate Control**

```
1:  procedure PROCESS FRP(FRP)
2:      CP = GetCP(FRP), FCP = GetF(FRP);
3:      if FRP.CP ∈ this.domain then
4:          if Qdev < Qth then
5:              r = FCP - (Qdev / N) × T;
6:          else
7:              γ = ScaleFactor / N;
8:              r = max(Fmin, FCP - γ × Qdev);
9:      else
10:         r = FCP;
11:     if r ≤ Rcur OR CPcur == CP then
12:         Rcur = r, CPcur = CP;
13:         F = FCP;
14:         Reset timer();
15: procedure TIME EXPIRE()
16:     if IsIntraDCFlow() then
17:         Rcur = Rcur × 2
18:     else
19:         Rcur = Rcur + ΔR;
20:     Reset timer();
21: function AUTOTUNE()
22:     N = Fc;
23:     if N < 1 then N = 1;
24:     if N > Nmax then N = Nmax;
25:     return N;
```

**Smooth rate adaptation for intra-DC flows.** Aggressive backoff of intra-DC flows ensures fast queue convergence but may destabilize the system around $Q_{\text{ref}}$, as cross-DC flows have not yet converged to the fair rate. In this regime, Eq. 4 can trigger premature rate increase of intra-DC flows, leading to queue re-accumulation. This effect is amplified by large $\gamma$, resulting in oscillations in both queue length and transmission rate.

We therefore introduce a smooth rate adaptation mechanism (Line 5) near the queue threshold. Specifically, the intra-DC transmission rate is adjusted as

$$
R_{\text{intra}} = F - \frac{Q_{\text{cur}} - Q_{\text{ref}}}{N_1 \times T} \tag{6}
$$

where $T$ denotes the FRP generation period. This formulation achieves smooth control over intra-DC transmission rates while maintaining stable queue dynamics around the threshold.

Packets traverse multiple switches, each generating FRP feedback, while the end-to-end rate is ultimately determined by the bottleneck switch. STEP identifies the bottleneck using a lightweight rule: the sender updates the bottleneck and rate when the computed sending rate falls below the current rate, or when the CP in FRP matches the previous bottleneck (Line 11–14).

A timeout-based mechanism is further introduced for fast rate recovery. Intra-DC flows use multiplicative increase for rapid adaptation (Line 17), while cross-DC flows adopt additive increase for stable rate ramp-up (Line 19).

### C. Acquisition of the fair rate

A natural approach is to update the fair rate at the switch based on the queue deviation until the queue stabilizes around $Q_{\text{ref}}$. In single-traffic scenarios, Eq. 7 converges quickly to the correct fair rate, where $F_{\text{old}}$ denotes the fair rate computed in the previous period.

$$
F = F_{\text{old}} - \alpha \times (Q_{\text{cur}} - Q_{\text{ref}}) \tag{7}
$$

However, under mixed traffic, senders apply differentiated control strategies (Section 3.2), which invalidates the direct application of Eq. 7 and requires an updated fair-rate estimation (Algorithm 2).

**Algorithm 2 Fair Rate Acquisition**

```
1:  function COMPUTE FAIR RATE() execute every T
2:      Qdev = Qcur - Qref;
3:      if Qdev > Qth then
4:          backoff = ScaleFactor × Qdev × T;
5:          v = backoff - Qold;
6:      else
7:          v = 0;
8:      F = Fold - α × (Qdev + v);
9:      if F < Fmin then
10:         F = Fmin;
11:     if F > Fmax then
12:         F = Fmax;
13:     return F;
```

Switches assume all flows operate at the fair rate $F$, and convergence is inferred when the queue stabilizes around $Q_{\text{ref}}$. However, under mixed traffic, intra-DC backoff and cross-DC lag coexist, making the physical queue insufficient to reflect true fairness convergence.

To compensate, we introduce a virtual byte term $v$ and estimate the fair rate as:

$$
F = F_{\text{old}} - \alpha \times (Q_{\text{cur}} - Q_{\text{ref}} + v) \tag{8}
$$

where $v$ captures the net imbalance between intra-DC backoff and cross-DC over-occupation. First, when severe queue buildup occurs at a mixed-traffic node, the intra-DC backoff contribution is given by:

$$
\text{backoff} = N_1 \times (F - R_{\text{intra}}) \times T \tag{9}
$$

By substituting the backoff coefficient $\gamma$ from Eq. 4, we obtain:

$$
\text{backoff} = \text{ScaleFactor} \times (Q_{\text{cur}} - Q_{\text{ref}}) \times T \tag{10}
$$

Interestingly, the intra-DC backoff volume depends solely on the queue length. For cross-DC over-occupation, we adopt a lightweight approximation. Ideally, aggressive intra-DC backoff should drain the entire accumulated queue; however, if residual queue $Q_{\text{old}}$ remains after the previous transmission epoch, it is attributed to cross-DC over-transmission.

When the queue stays within the threshold region, we treat the system as near convergence. In this regime, cross-DC flows lag while intra-DC flows follow smooth adaptation, and we therefore set $v = 0$. The virtual byte term is thus defined as follows:

$$
v = \begin{cases}
0, & Q_{\text{dev}} \leq Q_{\text{th}} \\
\text{backoff} - Q_{\text{old}}, & Q_{\text{dev}} > Q_{\text{th}}
\end{cases} \tag{11}
$$

Meanwhile, upper and lower bounds are applied to the fair rate to improve the robustness of the algorithm.

### D. System stability analysis

Based on STEP's differentiated rate control strategy, we first update the open-loop transfer function $G(s)$. Specifically, we have $c(s) = \frac{\alpha}{sT}$, and the frequency-domain representations of cross-DC and intra-DC flows are given by $R_{\text{cross}}(s) = c(s)E(s)$ and $R_{\text{intra}}(s) = (c(s) + \gamma)E(s)$, respectively. Substituting these expressions yields:

$$
G(s) = \frac{\kappa}{s} \left[ N_1 (c(s) + \gamma)e^{-sT_1} + N_2 c(s)e^{-sT_2} \right] \tag{12}
$$

It can be observed that the backoff parameter $\gamma$ strengthens the controllability of intra-DC flows. We next analyze the high frequency regime, where intra-DC flows dominate the dynamics. Due to significant phase delay in cross-DC flows, their impact can be neglected under burst conditions. Moreover, since $c(s) \to 0$ at high frequency, it is negligible compared to $\gamma$. Therefore, the fast-time-scale open-loop transfer function can be approximated as:

$$
G_{\text{fast}}(s) = \frac{\kappa N_1 \gamma e^{-sT_1}}{s} \tag{13}
$$

The backoff parameter $\gamma$ should not be chosen arbitrarily large. While a larger $\gamma$ accelerates queue convergence, it also increases the gain crossover frequency and reduces the phase margin, which can lead to oscillations in the sending rates of intra-DC flows. To suppress such oscillations, we follow standard control-design practice and adopt a conservative phase-margin target of $60^\circ$, which is commonly used to provide sufficient damping and reduce overshoot or ringing [18].

Applying this phase-margin constraint yields the following upper bound:

$$
\gamma \leq \frac{\pi}{6\kappa N_1 T_1} \tag{14}
$$

### E. Further discussion

STEP reduces cross-DC feedback delay by rapidly regulating intra-DC traffic, and thus focuses on the common case where intra-DC and cross-DC flows coexist at the bottleneck. A bottleneck shared only by cross-DC flows is less common in modern Fat-Tree or leaf-spine datacenter networks, where switches usually carry substantial local traffic and cross-DC traffic represents a smaller portion of the total demand [17].

Under this corner case, STEP degrades to explicit rate control with a feedback interval of $N_2$, but can still maintain high utilization. To further improve robustness, we discuss two possible extensions: (i) aggregating flows by destination IP at the source-side DCI switch to bound the total number of in-flight packets, and (ii) caching per-destination fair rates to initialize bursty cross-DC flows before fresh feedback arrives.

---

## IV. Implementation

**Switch implementation.** The switch performs two core functions: (i) forwarding data packets and (ii) periodically returning FRPs to source nodes. To comply with the physical pipeline isolation constraints of programmable switch ASICs, such as Intel Tofino, STEP decomposes the control logic across different pipeline stages (see Fig. 7).

*Fig. 7: Switch Pipeline of STEP.*

**Flow table update:** A straightforward design is to maintain exact per-flow state using exact-match flow tables, as in OpenFlow [19]; however, storing full five-tuple keys in SRAM/TCAM is costly and may suffer from hash conflicts, table overflow, and pipeline stalls under high concurrency. Recent systems therefore adopt approximate flow tables based on data sketches. Count-Min Sketch [20], Elastic Sketch [21], and HeavyKeeper [22] use compact hash-based data structures to approximate per-flow statistics and identify heavy hitters under tight memory budgets. This design is also aligned with commercial switch ASIC support: Broadcom BroadView Flow Tracker provides IPFIX-based flow monitoring by inspecting packets and exporting flow records [23].

**FRP generation:** FRP generation in the egress pipeline consists of two operations: (i) fair-rate computation based on metadata, and (ii) feedback packet reconstruction. After acquired the fair rate, it obtains the source IP and reverse egress port from the active-flow table, and uses P4 egress-to-egress mirroring to clone a CPU-injected packet template at line rate. The cloned packet is redirected to the reverse output port, and is rewritten in place during deparsing. The deparser reconstructs the packet by swapping source and destination addresses, inserting the FRP header, and encoding the computed fair rate before transmission.

**End-host implementation.** Modern RNICs provide hardware packet parsing and programmable data-path support, making STEP feasible at the end host. Commercial adapters such as NVIDIA ConnectX-6/7 support flexible parsing, match-action tables, and ASAP$^2$-based data-path offload, while Intel E810 supports Dynamic Device Personalization (DDP) for protocol-aware packet classification. These capabilities allow the NIC to parse standard or customized control packets and expose the required metadata to the rate-control logic.

---

## V. Evaluation

We evaluate the performance of STEP in both a real-world testbed and the NS-3 simulator. The key findings are as follows:

- STEP achieves high and stable performance under far-end congestion, near-end congestion, and hybrid scenarios with multiple congestion points. Compared with DCQCN, STEP reduces FCT by %.
- STEP gracefully handles different proportions of inter-domain and intra-domain flows. Even when the proportion of inter-domain flows is high, STEP enables the queue to converge rapidly, reducing the queue length by %, and decreases the overall average FCT by %.
- STEP is insensitive to transmission distance and eliminates the negative impact of long-haul propagation delay on system stability. In particular, at a distance of xx km, STEP reduces FCT by %.

### A. Testbed experiments

**Setup.** Our testbed consists of four hosts and two Tofino switches, each equipped with 32 200-Gbps ports and connected to two hosts. To emulate long-distance data transmission, we insert a delay-emulation host between the two switches and use DPDK to emulate the propagation delay of a long-haul link. Each host is equipped with an Intel E810 100GbE dual-port RNIC. Unless otherwise specified, we configure a 100 km long-haul link, corresponding to a one-way inter-switch propagation delay of 500 µs. The RNIC line rate is 100 Gbps, and the FRP generation interval $T$ is set to 40 µs. The fair rate reported by the switch is capped between 100 Mbps and 0.95 times the link bandwidth.

To keep FRPs compact, STEP uses coarse-grained field encoding so that a fixed number of bytes can represent a wider range of rate and queue values. Specifically, we use a rate granularity of 10 Mbps and a queue-length granularity of 600 B. Under this encoding, we set $\alpha$ to 0.1 and ScaleFactor to 20.

*Fig. 8: Three congestion scenarios in the testbed evaluation.*

**Far-end Congestion.** Figure 8(a) illustrates a scenario where the congestion point is located in the data center of the receiver-side host. Hosts A0 and B1 send flows to B0, causing congestion at switch CP1. We first evaluate STEP's ability to handle changes in the number of flows. Initially, there is one background flow from B1 to B0. At 10 ms, A0 starts transmitting an inter-DC flow to B0. Subsequently, B1 sends two additional intra-domain flows to B0. The transmission rate of each flow is shown in Figure 8, demonstrating that STEP can quickly respond to changes in the number of flows and converge to a fair bandwidth sharing state.

At the congestion point, two types of traffic coexist. The injection of inter-domain flows can increase system instability due to delayed feedback. To further analyze the performance of STEP, we vary the proportions of the two types of congesting traffic and compare STEP with DCQCN. As shown in Figure 9, STEP reduces FCT by %.

**Near-end Congestion.** Figure 8(b) illustrates a scenario where the congestion point is located in the data center of the sender-side host. In this scenario, once the near-source switch in STEP detects congestion, it immediately sends the fair rate back to the source host, allowing the inter-DC flow to reduce its sending rate within one intra-DC RTT. In contrast, DCQCN performs end-to-end rate control: it only marks packets at the near-source congestion point, and the actual rate reduction takes effect after a cross-DC RTT. Therefore, as shown in Figure 10, STEP reduces FCT by %.

**Hybrid Scenarios.** Figure 8(c) shows a more complex hybrid scenario, where congestion points exist in both data centers. In this scenario, STEP gains a clear advantage by allowing each switch to independently feed back its fair rate. The sender adjusts its sending rate according to the fair rate of the bottleneck congestion point, and promptly updates both the bottleneck point and the transmission rate. As shown in Figure 11, under hybrid congestion, STEP reduces FCT by % compared with DCQCN.

### B. Simulations

We conduct large-scale simulations in NS-3 using the topology shown in Figure 2. The topology adopts a fat-tree structure. The link bandwidth between DCI switches is set to 200 Gbps, and the corresponding propagation delay is configured according to different transmission distances. The link bandwidth between other nodes is set to 100 Gbps, with a propagation delay of 1.5 µs. In addition, we set the NIC reaction delay for feedback to 15 µs. We set $Q_{\text{th}}$ to 300 KB. $Q_{\text{ref}}$ is set to 500 KB for 100 Gbps links and 1 MB for 200 Gbps links. $\Delta R$ is set to 200 Mbps. We use AliStorage and WebSearch as workloads, and the remaining parameters are the same as those used in the testbed evaluation. In this section, we compare STEP with DCQCN, HPCC, ROCC, and ATC. The configurations of the baseline schemes follow the recommended parameters in [10, 24–26].

**Impact of the backoff parameter on FCT.** In Equation 4, intra-DC flows use the backoff parameter $\gamma$ to rapidly reduce their sending rates under congestion. A larger $\gamma$ allows the queue to drain more quickly toward the threshold, but it may also cause severe rate fluctuations for intra-DC flows and even reduce link utilization. In Section 3.4, we derived the upper bound of $\gamma$ that guarantees system stability. In practical transmission scenarios, $\gamma$ is computed from the ScaleFactor. Figure 12 shows the impact of different ScaleFactor values on flow completion time.

**Impact of the traffic mix on FCT.** The fundamental challenge in long-distance transmission stems from the delayed response of cross-DC flows, which can destabilize the system. To directly quantify the impact of introducing cross-DC traffic, we keep the total traffic load fixed and vary the ratio between intra-DC and cross-DC flows. Figure 13 shows how FCT changes under different proportions of inter-DC traffic for the AliStorage and WebSearch workloads.

**Impact of transmission distance on FCT.** We further evaluate the performance of each scheme under different transmission distances. As shown in Figure 14, ATC outperforms conventional data-center congestion control schemes because it shortens the control loop. STEP further reduces FCT by %, demonstrating its ability to mitigate the negative impact of long-haul propagation delay on transport performance.

**Queue analysis at congestion points.** We further analyze the queue length at the congestion points. Under mixed-traffic scenarios, we compare the mean and variance of the queue length across different schemes. As shown in Figure 15, STEP keeps the queue length stable around the target threshold, achieving both low queuing delay and high link utilization. During the entire simulation, STEP incurs no PFC triggering throughout the simulation.

---

## VI. Related Work

**Long-haul congestion control.** Recent work has explored congestion control for inter-datacenter transmission, where large RTTs and BDPs make feedback delayed and unstable. Swing [27] extends lossless RDMA to long-distance links by relaying PFC messages across the inter-DC path. Bifrost [12] introduces downstream-driven lossless flow control to reduce buffer reservation while maintaining lossless cross-DC transmission. ATC [10] places traffic control at the two DCI switches to separately handle local and remote congestion, thereby shortening the effective control loop. Torrent [11] re-architects end-to-end cross-DC RDMA transmission to improve long-haul link utilization and congestion convergence.

**Explicit rate control.** Switch-assisted explicit rate control schemes provide sources with richer feedback than binary congestion signals. QCN [28] measures congestion at Layer 2 switches and sends multi-bit feedback to sources. XCP [29] updates window-adjustment fields in packet headers at routers, with the feedback relayed by receivers. RCP [30] computes a fair-share rate at each link and lets packets carry the minimum rate along the path. TFC [31] uses switch-side token allocation based on the number of active flows. RoCC [26] employs a switch-side closed-loop controller and sends prioritized ICMP messages carrying rate feedback to congesting flows.

**Sender-side differentiated rate control.** Prior work has shown that senders can apply different rate-control behaviors based on traffic type, congestion location, or feedback signals. GTCP [32] switches cross-DC flows between sender-based and receiver-driven control when congestion is detected inside a datacenter. Annulus [33] separates WAN and datacenter traffic aggregates and uses a dual-loop design to rate-limit them when they share a near-source bottleneck. GEMINI [34] combines ECN and delay signals and adapts window dynamics according to RTT/BDP to handle DCN and WAN congestion.

---

## VII. Conclusion

We presented STEP, a switch-assisted congestion control mechanism for cross-datacenter RDMA transmission. STEP targets mixed intra-DC and cross-DC traffic by using switch-side fair-rate feedback and RTT-based differentiated rate updates at senders, thereby shortening the effective control loop and stabilizing bottleneck queues. Our analysis explains how cross-DC feedback delay degrades stability and guides the intra-DC backoff parameter. Testbed and NS-3 evaluations show that STEP reduces FCT, suppresses queue oscillations, and avoids PFC triggering.

---

## Acknowledgment

The preferred spelling of the word "acknowledgment" in America is without an "e" after the "g". Avoid the stilted expression "one of us (R. B. G.) thanks . . .". Instead, try "R. B. G. thanks. . .". Put sponsor acknowledgments in the unnumbered footnote on the first page.

---

## References

[1] C. Xie and B. Zhang, "Scaling optical interconnects for hyperscale data center networks," *Proceedings of the IEEE*, vol. 110, no. 11, pp. 1699–1713, 2022.

[2] L. Luo, H. Yu, K.-T. Foerster, M. Noormohammadpour, and S. Schmid, "Inter-datacenter bulk transfers: Trends and challenges," *IEEE Network*, vol. 34, no. 5, pp. 240–246, 2020.

[3] AWS, "Aws direct connect announces native 400 gbps dedicated connections at select locations," Jul. 2024, accessed: 2026-03-07. [Online]. Available: https://aws.amazon.com/about-aws/whats-new/2024/07/aws-direct-connect-native-400-gbps-dedicated-connections-select-locations/

[4] W. Bai, S. S. Abdeen, A. Agrawal, K. K. Attre, P. Bahl, A. Bhagat, G. Bhaskara, T. Brokhman, L. Cao, A. Cheema et al., "Empowering azure storage with {RDMA}," in *20th USENIX Symposium on Networked Systems Design and Implementation (NSDI 23)*, 2023, pp. 49–67.

[5] C. Guo, L. Yuan, D. Xiang, Y. Dang, R. Huang, D. Maltz, Z. Liu, V. Wang, B. Pang, H. Chen et al., "Pingmesh: A large-scale system for data center network latency measurement and analysis," in *Proceedings of the 2015 ACM Conference on Special Interest Group on Data Communication*, 2015, pp. 139–152.

[6] S. N. Avci, Z. Li, and F. Liu, "Congestion aware priority flow control in data center networks," in *2016 IFIP Networking Conference (IFIP Networking) and Workshops*. IEEE, 2016, pp. 126–134.

[7] B.-H. Oh, S. Vural, N. Wang, and R. Tafazolli, "Priority-based flow control for dynamic and reliable flow management in sdn," *IEEE Transactions on Network and Service Management*, vol. 15, no. 4, pp. 1720–1732, 2018.

[8] IEEE 802.1 Working Group, "802.1qbb - priority-based flow control," http://www.ieee802.org/1/pages/802.1bb.html, accessed: Jan. 2026.

[9] C. Guo, H. Wu, Z. Deng, G. Soni, J. Ye, J. Padhye, and M. Lipshteyn, "Rdma over commodity ethernet at scale," in *Proceedings of the 2016 ACM SIGCOMM Conference*, 2016, pp. 202–215.

[10] Z. Wan, J. Zhang, Y. Su, H. Pan, M. Yu, Y. Li, and T. Huang, "Re-architecting traffic control in cross-datacenter rdma networks," *IEEE Transactions on Networking*, 2025.

[11] H. Pan, Y. Li, Z. Wan, J. Zhang, and T. Huang, "Torrent: Re-architecting end-to-end transmission for cross-datacenter rdma networks," in *2025 IEEE Wireless Communications and Networking Conference (WCNC)*. IEEE, 2025, pp. 1–6.

[12] P. Yu, F. Xue, C. Tian, X. Wang, Y. Chen, T. Wu, L. Han, Z. Han, B. Wang, X. Gong et al., "Bifrost: Extending roce for long distance inter-dc links," in *2023 IEEE 31st International Conference on Network Protocols (ICNP)*. IEEE, 2023, pp. 1–12.

[13] M. Long, J. Han, W. Wang, J. Yang, and K. Xue, "Lscc: Link-segmented congestion control for rdma in cross-datacenter networks," in *2024 IEEE/ACM 32nd International Symposium on Quality of Service (IWQoS)*. IEEE, 2024, pp. 1–10.

[14] S. Jain, A. Kumar, S. Mandal, J. Ong, L. Poutievski, A. Singh, S. Venkata, J. Wanderer, J. Zhou, M. Zhu, J. Zolla, U. Hölzle, S. Stuart, and A. Vahdat, "B4: Experience with a Globally-Deployed Software Defined WAN," in *Proceedings of the ACM SIGCOMM Conference*, ser. SIGCOMM '13. ACM, 2013, pp. 3–14. [Online]. Available: https://doi.org/10.1145/2534169.2486019

[15] C.-Y. Hong, S. Kandula, R. Mahajan, M. Zhang, V. Gill, M. Nanduri, and R. Wattenhofer, "Achieving High Utilization with Software-Driven WAN," in *Proceedings of the ACM SIGCOMM Conference*, ser. SIGCOMM '13. ACM, 2013, pp. 15–26. [Online]. Available: https://doi.org/10.1145/2486001.2486012

[16] M. Denis, Y. Yao, A. Hatch, Q. Zhang, C. Lim, S. Zhang, K. Sugrue, H. Kwok, M. Jimenez Fernandez, P. Lapukhov, S. Hebbani, G. Nagarajan, O. Baldonado, L. Gao, and Y. Zhang, "EBB: Reliable and Evolvable Express Backbone Network in Meta," in *Proceedings of the ACM SIGCOMM Conference*, ser. SIGCOMM '23. ACM, 2023, pp. 346–359. [Online]. Available: https://doi.org/10.1145/3603269.3604860

[17] Z. Wang, Z. Li, G. Liu, Y. Chen, Q. Wu, and G. Cheng, "Examination of wan traffic characteristics in a large-scale data center network," in *Proceedings of the 21st ACM Internet Measurement Conference*, 2021, pp. 1–14.

[18] H. Zhang, "Understanding Power Supply Loop Stability and Compensation - Part 2: Unusual or Problematic Bode Plots," Jun. 2024, accessed: 2026-07-01. [Online]. Available: https://www.analog.com/en/resources/technical-articles/understanding-power-supply-loop-stability-and-compensation-part-2.html

[19] N. McKeown, T. Anderson, H. Balakrishnan, G. Parulkar, L. Peterson, J. Rexford, S. Shenker, and J. Turner, "Openflow: enabling innovation in campus networks," *ACM SIGCOMM computer communication review*, vol. 38, no. 2, pp. 69–74, 2008.

[20] G. Cormode and S. Muthukrishnan, "An improved data stream summary: the count-min sketch and its applications," *Journal of Algorithms*, vol. 55, no. 1, pp. 58–75, 2005.

[21] T. Yang, J. Jiang, P. Liu, Q. Huang, J. Gong, Y. Zhou, R. Miao, X. Li, and S. Uhlig, "Elastic sketch: Adaptive and fast network-wide measurements," in *Proceedings of the 2018 Conference of the ACM Special Interest Group on Data Communication*, 2018, pp. 561–575.

[22] T. Yang, H. Zhang, J. Li, J. Gong, S. Uhlig, S. Chen, and X. Li, "Heavykeeper: An accurate algorithm for finding top-k elephant flows," *IEEE/ACM Transactions on Networking*, vol. 27, no. 5, pp. 1845–1858, 2019.

[23] Broadcom Inc., "Flow tracker: Flow-based monitoring for improved network and application visibility," Broadcom Inc., Product Brief FlowT-PB100, 1 2018, accessed: 2026-06-28. [Online]. Available: https://docs.broadcom.com/docs/FlowT-PB100

[24] Y. Zhu, H. Eran, D. Firestone, C. Guo, M. Lipshteyn, Y. Liron, J. Padhye, S. Raindel, M. H. Yahia, and M. Zhang, "Congestion control for large-scale rdma deployments," *ACM SIGCOMM Computer Communication Review*, vol. 45, no. 4, pp. 523–536, 2015.

[25] Y. Li, R. Miao, H. H. Liu, Y. Zhuang, F. Feng, L. Tang, Z. Cao, M. Zhang, F. Kelly, M. Alizadeh et al., "Hpcc: High precision congestion control," in *Proceedings of the ACM special interest group on data communication*, 2019, pp. 44–58.

[26] P. Taheri, D. Menikkumbura, E. Vanini, S. Fahmy, P. Eugster, and T. Edsall, "Rocc: robust congestion control for rdma," in *Proceedings of the 16th International conference on emerging networking experiments and technologies*, 2020, pp. 17–30.

[27] Y. Chen, C. Tian, J. Dong, S. Feng, X. Zhang, C. Liu, P. Yu, N. Xia, W. Dou, and G. Chen, "Swing: Providing Long-Range Lossless RDMA via PFC-Relay," *IEEE Transactions on Parallel and Distributed Systems*, vol. 34, no. 1, pp. 63–75, 2023. [Online]. Available: https://doi.org/10.1109/TPDS.2022.3215517

[28] M. Alizadeh, B. Atikoglu, A. Kabbani, A. Lakshmikantha, R. Pan, B. Prabhakar, and M. Seaman, "Data center transport mechanisms: Congestion control theory and ieee standardization," in *2008 46th Annual Allerton Conference on Communication, Control, and Computing*. IEEE, 2008, pp. 1270–1277.

[29] D. Katabi, M. Handley, and C. Rohrs, "Congestion control for high bandwidth-delay product networks," in *Proceedings of the ACM SIGCOMM Conference*, ser. SIGCOMM '02. ACM, 2002, pp. 89–102. [Online]. Available: https://doi.org/10.1145/633025.633035

[30] C.-H. Tai, J. Zhu, and N. Dukkipati, "Making Large Scale Deployment of RCP Practical for Real Networks," in *Proceedings of the IEEE International Conference on Computer Communications*, ser. INFOCOM '08. IEEE, 2008. [Online]. Available: https://doi.org/10.1109/INFOCOM.2008.285

[31] J. Zhang, F. Ren, R. Shu, and P. Cheng, "TFC: Token Flow Control in Data Center Networks," in *Proceedings of the European Conference on Computer Systems*, ser. EuroSys '16. ACM, 2016. [Online]. Available: https://doi.org/10.1145/2901318.2901336

[32] S. Zou, J. Huang, J. Liu, T. Zhang, N. Jiang, and J. Wang, "GTCP: Hybrid Congestion Control for Cross-Datacenter Networks," in *Proceedings of the IEEE International Conference on Distributed Computing Systems*, ser. ICDCS '21. IEEE, 2021. [Online]. Available: https://doi.org/10.1109/ICDCS51616.2021.00032

[33] A. Saeed, V. Gupta, P. Goyal, M. Sharif, R. Pan, M. Ammar, E. Zegura, K. Jang, M. Alizadeh, A. Kabbani, and A. Vahdat, "Annulus: A Dual Congestion Control Loop for Datacenter and WAN Traffic Aggregates," in *Proceedings of the Annual Conference of the ACM Special Interest Group on Data Communication on the Applications, Technologies, Architectures, and Protocols for Computer Communication*, ser. SIGCOMM '20. ACM, 2020, pp. 735–749. [Online]. Available: https://doi.org/10.1145/3387514.3405899

[34] G. Zeng, W. Bai, G. Chen, K. Chen, D. Han, Y. Zhu, and L. Cui, "Congestion control for cross-datacenter networks," *IEEE/ACM Transactions on Networking*, vol. 30, no. 5, pp. 2074–2089, 2022.
