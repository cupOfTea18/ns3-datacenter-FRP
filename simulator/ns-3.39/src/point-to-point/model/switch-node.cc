#include "ns3/ipv4.h"
#include <iomanip>
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
#include "ns3/pause-header.h"
#include "ns3/interface-tag.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "switch-node.h"
#include "qbb-net-device.h"
#include "qbb-channel.h"
#include "ppp-header.h"
#include "ns3/int-header.h"
#include "ns3/simulator.h"
#include <cmath>
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"
#include "ns3/custom-priority-tag.h"
#include "ns3/feedback-tag.h"
#include "ns3/unsched-tag.h"
#include "ns3/icmpv4.h"  // 添加ICMP FRP支持

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("SwitchNode");

TypeId SwitchNode::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::SwitchNode")
	                    .SetParent<Node> ()
	                    .AddConstructor<SwitchNode> ()
	                    .AddAttribute("EcnEnabled",
	                                  "Enable ECN marking.",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&SwitchNode::m_ecnEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("CcMode",
	                                  "CC mode.",
	                                  UintegerValue(0),
	                                  MakeUintegerAccessor(&SwitchNode::m_ccMode),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("AckHighPrio",
	                                  "Set high priority for ACK/NACK or not",
	                                  UintegerValue(0),
	                                  MakeUintegerAccessor(&SwitchNode::m_ackHighPrio),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("MaxRtt",
	                                  "Max Rtt of the network",
	                                  UintegerValue(9000),
	                                  MakeUintegerAccessor(&SwitchNode::m_maxRtt),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("PowerEnabled",
	                                  "Inserts Rxbytes instead of Txbytes in INT header",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&SwitchNode::PowerEnabled),
	                                  MakeBooleanChecker())
	                    
	                    // ========== Bifrost (ccMode=12) 属性 ==========
	                    .AddAttribute("BifrostEnabled",
	                                  "Enable Bifrost periodic PFC mechanism (only for designated switch)",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&SwitchNode::m_bifrostEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("BifrostPfcPeriodUs",
	                                  "Bifrost PFC sending period in microseconds",
	                                  UintegerValue(10),
	                                  MakeUintegerAccessor(&SwitchNode::m_bifrostPfcPeriodUs),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("BifrostDeploySwitchId",
	                                  "Switch ID where Bifrost is deployed",
	                                  UintegerValue(13),
	                                  MakeUintegerAccessor(&SwitchNode::m_bifrostDeploySwitchId),
	                                  MakeUintegerChecker<uint32_t>())
	                    
	                    // ========== 通用反馈包属性 ==========
	                    .AddAttribute("SwitchFeedbackEnabled",
	                                  "Enable switch periodic feedback mechanism",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&SwitchNode::m_switchFeedbackEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("FeedbackInterval",
	                                  "Feedback generation interval",
	                                  TimeValue(MicroSeconds(100)),
	                                  MakeTimeAccessor(&SwitchNode::m_feedbackInterval),
	                                  MakeTimeChecker())
	
	                    ;
	return tid;
}

SwitchNode::SwitchNode() {
	m_ecmpSeed = m_id;
	m_node_type = 1;
	m_mmu = CreateObject<SwitchMmu>();
	for (uint32_t i = 0; i < pCnt; i++)
		for (uint32_t j = 0; j < pCnt; j++)
			for (uint32_t k = 0; k < qCnt; k++)
				m_bytes[i][j][k] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_txBytes[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_lastPktSize[i] = m_lastPktTs[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_u[i] = 0;
	

	m_atcGateway.setSwitchSendToDevCallback(MakeCallback(&SwitchNode::SendToDev,this));
	m_atcGateway.setGetOutDevCallback(MakeCallback(&SwitchNode::GetOutDevice,this));
	// ========== 通用反馈包初始化 ==========
	m_switchFeedbackEnabled = false;
	m_feedbackInterval = MicroSeconds(10);
	m_activeFlows.clear();
	m_activeSrcDcIds.clear();
	m_switchRealIp = Ipv4Address("0.0.0.0");  // 默认IP

	// ========== Bifrost (ccMode=12) 初始化 ==========
	m_bifrostEnabled = false;
	m_bifrostPfcPeriodUs = 10;
	m_bifrostLastPfcTimeUs = 0;
	m_bifrostPfcEvent = EventId();
}

int SwitchNode::GetOutDev(Ptr<const Packet> p, CustomHeader &ch) {
	// look up entries
	Ptr<Packet> cp = p->Copy();

	PppHeader ph; cp->RemoveHeader(ph);
	Ipv4Header ih; cp->RemoveHeader(ih);
	auto entry = m_rtTable.find(ih.GetDestination().Get());

	// no matching entry
	if (entry == m_rtTable.end())
		return -1;

	// entry found
	auto &nexthops = entry->second;

	// pick one next hop based on hash
	union {
		uint8_t u8[4 + 4 + 2 + 2];
		uint32_t u32[3];
	} buf{};
	buf.u32[0] = ih.GetSource().Get();
	buf.u32[1] = ih.GetDestination().Get();
	if (ih.GetProtocol() == 0x6) {
		TcpHeader th; cp->PeekHeader(th);
		buf.u32[2] = th.GetSourcePort() | ((uint32_t)th.GetDestinationPort() << 16);
	}
	else if (ch.l3Prot == 0x11) {
		buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
	}
	else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD)
		buf.u32[2] = ch.ack.sport | ((uint32_t)ch.ack.dport << 16);

	uint32_t idx = EcmpHash(buf.u8, 12, m_ecmpSeed) % nexthops.size();
	// if (nexthops.size()>1){ std::cout << "selected " << idx << std::endl; }
	return nexthops[idx];
}

Ptr<QbbNetDevice>  SwitchNode::GetOutDevice(Ptr< Packet> p, CustomHeader &ch) {
	int idx = GetOutDev(p, ch);
	if (idx >= 0) {
		return DynamicCast<QbbNetDevice>(m_devices[idx]);
	}

	return nullptr;
}

void SwitchNode::CheckAndSendPfc(uint32_t inDev, uint32_t qIndex) {
    if (m_bifrostEnabled && inDev == m_bifrostIngress && qIndex == m_bifrostPriority) return;
	if (m_mmu->CheckShouldPause(inDev, qIndex)) {
		m_mmu->SetPause(inDev, qIndex);
		RefreshPfc(inDev, qIndex);
	}
}

void SwitchNode::RefreshPfc(uint32_t inDev, uint32_t qIndex) {
	// The peer expires each pause. Renew while the MMU still requests it,
	// including periods with no new arrivals. Bifrost owns its separate timer.
	auto device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	UintegerValue duration;
	device->GetAttribute("PauseTime", duration);
	NS_ABORT_MSG_IF(duration.Get() == 0, "ordinary PFC requires a positive PauseTime");
	device->SendPfc(qIndex, 0);
	m_pfcRefresh[{inDev, qIndex}] = Simulator::Schedule(
		NanoSeconds(duration.Get() * 1000 / 2), &SwitchNode::RefreshPfc, this, inDev, qIndex);
}

void SwitchNode::DoDispose() {
	for (const auto& entry : m_pfcRefresh) Simulator::Cancel(entry.second);
	m_pfcRefresh.clear();
	Node::DoDispose();
}

void SwitchNode::CheckAndSendResume(uint32_t inDev, uint32_t qIndex) {
    if (m_bifrostEnabled && inDev == m_bifrostIngress && qIndex == m_bifrostPriority) return;
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldResume(inDev, qIndex)) {
		auto pending = m_pfcRefresh.find({inDev, qIndex});
		if (pending != m_pfcRefresh.end()) {
			Simulator::Cancel(pending->second);
			m_pfcRefresh.erase(pending);
		}
		device->SendPfc(qIndex, 1);
		m_mmu->SetResume(inDev, qIndex);
	}
}

void SwitchNode::SendToDev(Ptr<Packet>p, CustomHeader &ch) {
	int idx = GetOutDev(p, ch);
	if (idx >= 0) {
		NS_ASSERT_MSG(m_devices[idx]->IsLinkUp(), "The routing table look up should return link that is up");

		// determine the qIndex
		uint32_t qIndex=0;
		MyPriorityTag priotag;
		// IMPORTANT: MyPriorityTag should only be attached by lossy traffic. This tag indicates the qIndex but also indicates that it is "lossy". Never attach MyPriorityTag on lossless traffic.
		bool found = p->PeekPacketTag(priotag);

		// UnSchedTag is used by ABM. End-hosts explicitly tag packets of the first BDP so that ABM then prioritizes these packets in the buffer allocation.
		uint32_t unsched = 0;
		UnSchedTag tag;
		bool foundunSched = p->PeekPacketTag (tag);
		if (foundunSched) {
			unsched = tag.GetValue();
		}

		if (ch.l3Prot == 0xF9 || ch.l3Prot == 0xFF || ch.l3Prot == 0xFE || (m_ackHighPrio && (ch.l3Prot == 0xFD || ch.l3Prot == 0xFC))) { //QCN or PFC or NACK, go highest priority
			qIndex = 0;
		}
		else if (found) {
			qIndex = priotag.GetPriority();
			// std::cout << "using queue " << qIndex << std::endl;
		}
		else {
			qIndex = (ch.l3Prot == 0x06 ? 1 : ch.udp.pg); // For TCP/IP if the stack did not attach MyPriorityTag, put to queue 1.
		}

		// admission control
		InterfaceTag t;
		bool hasIngressTag = p->PeekPacketTag(t);
		uint32_t inDev = hasIngressTag ? t.GetPortId() : idx;
			
        if (m_bifrostEnabled && inDev == m_bifrostIngress && qIndex == m_bifrostPriority)
            m_bifrostReceived += p->GetSize();

		if (qIndex != 0 && hasIngressTag) { //not highest priority
			// IMPORTANT: MyPriorityTag should only be attached by lossy traffic. This tag indicates the qIndex but also indicates that it is "lossy". Never attach MyPriorityTag on lossless traffic.
			if (m_mmu->CheckIngressAdmission(inDev, qIndex, p->GetSize(), found,unsched) && m_mmu->CheckEgressAdmission(idx, qIndex, p->GetSize(), found,unsched)) {			// Admission control
				m_mmu->UpdateIngressAdmission(inDev, qIndex, p->GetSize(), found, unsched);
				m_mmu->UpdateEgressAdmission(idx, qIndex, p->GetSize(), found);
			} else {
				++m_admissionDropPackets;
				return; // Drop
			}
			CheckAndSendPfc(inDev, qIndex);
		}
		if (inDev < m_devices.size()) {
			m_bytes[inDev][idx][qIndex] += p->GetSize();
		}
		m_devices[idx]->SwitchSend(qIndex, p, ch);
		DynamicCast<QbbNetDevice>(m_devices[idx])->totalBytesRcvd += p->GetSize(); // Attention: this is the egress port's total received packets. Not the ingress port.
	} else
		std::cout << "outdev not found! Dropped. This should not happen. Debugging required!" << std::endl;
	return; // Drop
}

uint32_t SwitchNode::EcmpHash(const uint8_t* key, size_t len, uint32_t seed) {
	uint32_t h = seed;
	if (len > 3) {
		const uint32_t* key_x4 = (const uint32_t*) key;
		size_t i = len >> 2;
		do {
			uint32_t k = *key_x4++;
			k *= 0xcc9e2d51;
			k = (k << 15) | (k >> 17);
			k *= 0x1b873593;
			h ^= k;
			h = (h << 13) | (h >> 19);
			h += (h << 2) + 0xe6546b64;
		} while (--i);
		key = (const uint8_t*) key_x4;
	}
	if (len & 3) {
		size_t i = len & 3;
		uint32_t k = 0;
		key = &key[i - 1];
		do {
			k <<= 8;
			k |= *key--;
		} while (--i);
		k *= 0xcc9e2d51;
		k = (k << 15) | (k >> 17);
		k *= 0x1b873593;
		h ^= k;
	}
	h ^= len;
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}

void SwitchNode::SetEcmpSeed(uint32_t seed) {
	m_ecmpSeed = seed;
}

int SwitchNode::LookupOutputPort(Ptr<const Packet> packet, CustomHeader &header) {
    return GetOutDev(packet, header);
}

void SwitchNode::SendNetworkControl(Ptr<Packet> packet) {
    CustomHeader header(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(header);
    NS_ABORT_MSG_IF(header.l3Prot != 0xff && header.l3Prot != 0xf9,
                    "only CNP/Proposed STATE may originate through this interface");
    NS_ABORT_MSG_IF(GetOutDev(packet, header) < 0, "no route for generated control packet");
    SendToDev(packet, header);
}

void SwitchNode::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx) {
	uint32_t dip = dstAddr.Get();
	m_rtTable[dip].push_back(intf_idx);
}

void SwitchNode::ClearTable() {
	m_rtTable.clear();
}

// This function can only be called in switch mode
bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch) {
	// atcGateway处理
	bool ret = m_atcGateway.packetIn(device, packet, ch);
	if (ret)
	{
		return true;
	}
	
	SendToDev(packet, ch);
	return true;
}

void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p) {
	InterfaceTag t;
	p->PeekPacketTag(t);

	// Per-packet queue debug output is intentionally disabled.  Long-haul
	// experiments sample queues in their structured DCI CSV; printing one line
	// for every dequeue can turn a run into hundreds of megabytes of stdout.

	MyPriorityTag priotag;
	bool found = p->PeekPacketTag(priotag);

	if (qIndex != 0) {
		uint32_t inDev = t.GetPortId();
		m_mmu->RemoveFromIngressAdmission(inDev, qIndex, p->GetSize(), found);
		m_mmu->RemoveFromEgressAdmission(ifIndex, qIndex, p->GetSize(), found);
		m_bytes[inDev][ifIndex][qIndex] -= p->GetSize();
        if (m_ecnEnabled && ShouldMarkEcn(ifIndex, p))
        {
            bool egressCongested = m_mmu->ShouldSendCN(ifIndex, qIndex);
            if (egressCongested)
            {
                PppHeader ppp;
                Ipv4Header h;
                p->RemoveHeader(ppp);
                p->RemoveHeader(h);
                h.SetEcn((Ipv4Header::EcnType)0x03);
                p->AddHeader(h);
                p->AddHeader(ppp);
                if (m_atcGateway.m_gatewayType == 1)
                {                                    // ← 新增
                    m_atcGateway.m_congested = true; // ←
                }
            }
			else
			{
				if (m_atcGateway.m_gatewayType == 1)
				{
					m_atcGateway.m_congested = false;
				}
			}
        }
        //CheckAndSendPfc(inDev, qIndex);
		CheckAndSendResume(inDev, qIndex);
	}
	if (1) {
		uint8_t* buf = p->GetBuffer();
		if (buf[PppHeader::GetStaticSize() + 9] == 0x11) { // udp packet
			IntHeader *ih = (IntHeader*)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6]; // ppp, ip, udp, SeqTs, INT
			Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
			if (m_ccMode == 3) { // HPCC or PowerTCP-INT
				if (!PowerEnabled)
					ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
				else
					ih->PushHop(Simulator::Now().GetTimeStep(), dev->GetQueue()->GetNBytesRxTotal(), dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
				// ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
			} else if (m_ccMode == 10) { // HPCC-PINT
				uint64_t t = Simulator::Now().GetTimeStep();
				uint64_t dt = t - m_lastPktTs[ifIndex];
				if (dt > m_maxRtt)
					dt = m_maxRtt;
				uint64_t B = dev->GetDataRate().GetBitRate() / 8; //Bps
				uint64_t qlen = dev->GetQueue()->GetNBytesTotal();
				double newU;

				/**************************
				 * approximate calc
				 *************************/
				int b = 20, m = 16, l = 20; // see log2apprx's paremeters
				int sft = logres_shift(b, l);
				double fct = 1 << sft; // (multiplication factor corresponding to sft)
				double log_T = log2(m_maxRtt) * fct; // log2(T)*fct
				double log_B = log2(B) * fct; // log2(B)*fct
				double log_1e9 = log2(1e9) * fct; // log2(1e9)*fct
				double qterm = 0;
				double byteTerm = 0;
				double uTerm = 0;
				if ((qlen >> 8) > 0) {
					int log_dt = log2apprx(dt, b, m, l); // ~log2(dt)*fct
					int log_qlen = log2apprx(qlen >> 8, b, m, l); // ~log2(qlen / 256)*fct
					qterm = pow(2, (
					                log_dt + log_qlen + log_1e9 - log_B - 2 * log_T
					            ) / fct
					           ) * 256;
					// 2^((log2(dt)*fct+log2(qlen/256)*fct+log2(1e9)*fct-log2(B)*fct-2*log2(T)*fct)/fct)*256 ~= dt*qlen*1e9/(B*T^2)
				}
				if (m_lastPktSize[ifIndex] > 0) {
					int byte = m_lastPktSize[ifIndex];
					int log_byte = log2apprx(byte, b, m, l);
					byteTerm = pow(2, (
					                   log_byte + log_1e9 - log_B - log_T
					               ) / fct
					              );
					// 2^((log2(byte)*fct+log2(1e9)*fct-log2(B)*fct-log2(T)*fct)/fct) ~= byte*1e9 / (B*T)
				}
				if (m_maxRtt > dt && m_u[ifIndex] > 0) {
					int log_T_dt = log2apprx(m_maxRtt - dt, b, m, l); // ~log2(T-dt)*fct
					int log_u = log2apprx(int(round(m_u[ifIndex] * 8192)), b, m, l); // ~log2(u*512)*fct
					uTerm = pow(2, (
					                log_T_dt + log_u - log_T
					            ) / fct
					           ) / 8192;
					// 2^((log2(T-dt)*fct+log2(u*512)*fct-log2(T)*fct)/fct)/512 = (T-dt)*u/T
				}
				newU = qterm + byteTerm + uTerm;

#if 0
				/**************************
				 * accurate calc
				 *************************/
				double weight_ewma = double(dt) / m_maxRtt;
				double u;
				if (m_lastPktSize[ifIndex] == 0)
					u = 0;
				else {
					double txRate = m_lastPktSize[ifIndex] / double(dt); // B/ns
					u = (qlen / m_maxRtt + txRate) * 1e9 / B;
				}
				newU = m_u[ifIndex] * (1 - weight_ewma) + u * weight_ewma;
				printf(" %lf\n", newU);
#endif

				/************************
				 * update PINT header
				 ***********************/
				uint16_t power = Pint::encode_u(newU);
				if (power > ih->GetPower())
					ih->SetPower(power);

				m_u[ifIndex] = newU;
			}
		}
		else {
			FeedbackTag Int;
			bool found;
			found = p->PeekPacketTag(Int);
			if (found) {
				Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
				Int.setTelemetryQlenDeq(Int.getHopCount(), dev->GetQueue()->GetNBytesTotal()); // queue length at dequeue
				Int.setTelemetryTsDeq(Int.getHopCount(), Simulator::Now().GetNanoSeconds()); // timestamp at dequeue
				Int.setTelemetryBw(Int.getHopCount(), dev->GetDataRate().GetBitRate());
				Int.setTelemetryTxBytes(Int.getHopCount(), m_txBytes[ifIndex]);
				Int.incrementHopCount(); // Incrementing hop count at Dequeue. Don't do this at enqueue.
				p->ReplacePacketTag(Int); // replacing the tag with new values
				// std::cout << "found " << Int.getHopCount() << std::endl;
			}
		}
	}
	
	// ========== 通用反馈包：跟踪活跃流 ==========
	if (m_switchFeedbackEnabled) {
		TrackActiveFlow(p, ifIndex);
	}
	
	m_txBytes[ifIndex] += p->GetSize();
	m_lastPktSize[ifIndex] = p->GetSize();
	m_lastPktTs[ifIndex] = Simulator::Now().GetTimeStep();
}

int SwitchNode::logres_shift(int b, int l) {
	static int data[] = {0, 0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
	return l - data[b];
}

int SwitchNode::log2apprx(int x, int b, int m, int l) {
	int x0 = x;
	int msb = int(log2(x)) + 1;
	if (msb > m) {
		x = (x >> (msb - m) << (msb - m));
#if 0
		x += + (1 << (msb - m - 1));
#else
		int mask = (1 << (msb - m)) - 1;
		if ((x0 & mask) > (rand() & mask))
			x += 1 << (msb - m);
#endif
	}
	return int(log2(x) * (1 << logres_shift(b, l)));
}

/* =========================================================================
 * 统一控制包配置与发送框架（去协议化设计）
 * ========================================================================= */

/**
 * 检查流是否有广域流竞争
 * 判断依据：交换机IP和源IP的第2段（数据中心ID）是否不同
 * 
 * IP格式: 11.{dc_id}.{id/256}.{id%256}
 * 子网掩码: 255.255.0.0 (前两段相同 = 同一DC)
 * 
 * @param srcIp 流的源IP地址
 * @return true 如果源IP和交换机属于不同数据中心（广域流）
 */


/**
 * 跟踪活跃流：在数据包出队时记录源IP和流信息
 * 作用：维护每个出口网卡的活跃源节点集合
 */
void SwitchNode::TrackActiveFlow(Ptr<Packet> p, uint32_t outPort) {
    if (!m_switchFeedbackEnabled) return;

    CustomHeader flowHeader(CustomHeader::L2_Header | CustomHeader::L3_Header |
                            CustomHeader::L4_Header);
    flowHeader.getInt = 0;
    p->PeekHeader(flowHeader);

    uint8_t proto = flowHeader.l3Prot;
    Ipv4Address srcIp(flowHeader.sip);
    Ipv4Address dstIp(flowHeader.dip);

    // 调试：打印所有包的协议号
    static uint32_t dbgCount = 0;
    if (dbgCount < 5) {
        std::cout << "[TRACK DEBUG] Switch " << m_id << " port=" << outPort
                  << " proto=0x" << std::hex << (uint32_t)proto << std::dec
                  << " src=" << srcIp << " dst=" << dstIp << std::endl;
        dbgCount++;
    }

    // 只跟踪UDP/TCP数据流
    if (proto == 0x11 || proto == 0x06) {
        // 记录流端点 (srcIp=数据流源IP, dstIp=数据流目的IP)
        FlowEndpoints fe;
        fe.srcIp = srcIp;
        fe.dstIp = dstIp;
        fe.sport = proto == 0x11 ? flowHeader.udp.sport : flowHeader.tcp.sport;
        fe.pg = proto == 0x11 ? flowHeader.udp.pg : 0;
        m_activeFlows[outPort].insert(fe);
		if (m_ccMode == 14)
			m_roccPorts.insert(outPort);

        // 提取源IP的DC-ID（IP第2段：11.{dc_id}.*.*）
        uint32_t srcIpValue = srcIp.Get();
        uint8_t srcDcId = (srcIpValue >> 16) & 0xFF;
        m_activeSrcDcIds[outPort].insert(srcDcId);

        // 调试：如果该端口有多个DC的流，输出提示
        if (m_activeSrcDcIds[outPort].size() > 1) {
            std::cout << "  [WAN DETECT] Switch " << m_id << " port=" << outPort
                      << " has multi-DC flows! src=" << srcIp << " (dc=" << (int)srcDcId << ")";
            std::cout << " DCs on port=";
            for (auto dc : m_activeSrcDcIds[outPort]) std::cout << (int)dc << ",";
            std::cout << std::endl;
        }
    }
}

/**
 * 统一公开接口：启动周期性反馈机制
 * 作用：在仿真开始时调用一次，启动周期驱动器
 */
void SwitchNode::StartPeriodicFeedbackMechanism(Time interval) {
    if (!m_switchFeedbackEnabled) {
        NS_LOG_INFO("Switch " << m_id << ": Feedback mechanism disabled");
        return;
    }
    
    NS_LOG_INFO("Switch " << m_id << ": Starting periodic feedback with interval " 
              << interval.As(Time::US));
    
    m_feedbackInterval = interval;
    Simulator::Schedule(interval, &SwitchNode::PeriodicFeedbackLoop, this, interval);
}

/**
 * 周期驱动器：纯粹的调度员
 * 作用：管理时间、筛选目标、触发任务函数
 */
void SwitchNode::PeriodicFeedbackLoop(Time interval) {
    if (!m_switchFeedbackEnabled) return;
    
    // 使用交换机的真实IP作为源IP（仅用于路由查找）
    Ipv4Address switchIp = m_switchRealIp;
    if (switchIp == Ipv4Address("0.0.0.0")) {
        uint32_t switchIpValue = (10 << 24) | (0 << 16) | (m_id << 8) | 254;
        switchIp = Ipv4Address(switchIpValue);
    }
    
    // 统计活跃流
    uint32_t totalActiveFlows = 0;
    for (auto const& [idx, flowSet] : m_activeFlows) {
        totalActiveFlows += flowSet.size();
    }
    if (totalActiveFlows > 0) {
        std::cout << "[FRP SEND] Switch " << m_id << " t=" << Simulator::Now().GetSeconds() 
                  << " activeFlows=" << totalActiveFlows << std::endl;
    }
    
    // RoCC updates every known congestion point each period, even when that
    // period has no feedback recipient. This lets an idle queue drive its fair
    // rate back toward line rate, as required by the periodic CP algorithm.
    std::map<uint32_t, Ptr<Packet>> roccPayloads;
    if (m_ccMode == 14) {
        for (uint32_t idx : m_roccPorts) {
            roccPayloads[idx] = ConfigureFeedbackPayload(m_ccMode, idx);
        }
    }

    // 轮询当前周期内所有产生过流量的物理出口网卡
    for (auto const& [idx, flowSet] : m_activeFlows) {
        if (flowSet.empty()) continue;
        
            
        // 1. 调用配置函数获取组装好的"纯净控制包"
        Ptr<Packet> controlPayload = m_ccMode == 14
            ? roccPayloads.at(idx)
            : ConfigureFeedbackPayload(m_ccMode, idx);
            
        // 2. 遍历该网卡上的所有活跃流，批量把控制包投递回去
        for (auto const& fe : flowSet) {
            // FRP包IP封装：
            //   SIP = fe.dstIp (数据流的目的IP，帮助接收方匹配QP)
            //   DIP = fe.srcIp (数据流的源IP，即发送方，FRP包发给他)
            Ipv4Address frpSrcAddr = fe.dstIp;
            Ipv4Address frpDstAddr = fe.srcIp;

            // 查路由表验证转发端口
            auto routeEntry = m_rtTable.find(frpDstAddr.Get());
            std::string routeInfo = "NO_ROUTE";
            if (routeEntry != m_rtTable.end() && !routeEntry->second.empty()) {
                routeInfo = "ports=[";
                for (size_t pi = 0; pi < routeEntry->second.size(); pi++) {
                    if (pi > 0) routeInfo += ",";
                    routeInfo += std::to_string(routeEntry->second[pi]);
                }
                routeInfo += "]";
            }

            std::cout << "  [FRP SEND] port=" << idx << " -> srcIp=" << fe.srcIp
                      << " FRP<SIp=" << frpSrcAddr << ", DIp=" << frpDstAddr << ">"
                      << " route:" << routeInfo << std::endl;

            // 3. 调用发送函数完成发送，按 DIP 查路由表转发
            SendControlPacket(frpSrcAddr, frpDstAddr, controlPayload, fe.sport, fe.pg);
        }
    }
    
    // 账本清空，续订下一周期
    m_activeFlows.clear();
    m_activeSrcDcIds.clear();
    Simulator::Schedule(interval, &SwitchNode::PeriodicFeedbackLoop, this, interval);
}

/**
 * 核心任务函数 1：根据ccMode动态装配控制包载荷
 * 作用：SwitchNode不关心具体协议，只根据ccMode调用对应的头部类
 */
Ptr<Packet> SwitchNode::ConfigureFeedbackPayload(uint32_t ccMode,
                                                  uint32_t ifIndex) {
    Ptr<Packet> payload = Create<Packet>();
    Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
    
    // 调试日志：输出ccMode
    static bool logged = false;
    if (!logged && (ccMode == 13 || ccMode == 14)) {
        fprintf(stderr, "[SWITCH_DEBUG] Switch %u ConfigureFeedbackPayload ccMode=%u ifIndex=%u\n", m_id, ccMode, ifIndex);
        logged = true;
    }
    
    if (!dev) {
        NS_LOG_WARN("Invalid device at port " << ifIndex);
        return payload;
    }
    
    uint32_t currentQDepth = dev->GetQueue()->GetNBytesTotal();  // 标准单位: Byte
    uint64_t link_bps = dev->GetDataRate().GetBitRate();         // 标准单位: bps
    

    
    // 根据ccMode装配不同的控制包
    if (ccMode == 13 || ccMode == 14) {
        // ========== FRP 模式(13是ours, 14是ROCC) ==========

        // ===== 跨域流连续识别 (FRP 专用) =====
        // 1. 读出上周期累计的计数器和上周期队列深度
        const uint32_t CROSS_DC_CONSEC_LIMIT = 10;
        uint32_t crossDcCounter = m_crossDcCounter[ifIndex];
        uint32_t prevQBytes    = m_prevQBytes[ifIndex];

        // 2. 本轮采样转换成 cell 单位，与 frp-rate-calculator.cc 保持一致
        double qRefBytesLocal;
        if (link_bps >= 200ULL * 1000000000ULL) {
            qRefBytesLocal = g_frpParameters.qref200Bytes;  // 1MB (200Gbps)
        } else if (link_bps >= 100ULL * 1000000000ULL) {
            qRefBytesLocal = g_frpParameters.qrefBytes;   // 300KB (100Gbps)
        } else {
            qRefBytesLocal = g_frpParameters.qrefBytes;   // 300KB (40Gbps)
        }
        const double qThBytesLocal  = g_frpParameters.thresholdBytes;   // 300KB 与 frp-rate-calculator.cc 一致
        const double qCurCellLocal  = static_cast<double>(currentQDepth) / 600.0;
        const double qRefCellLocal  = qRefBytesLocal / 600.0;
        const double qThCellLocal   = qThBytesLocal / 600.0;
        const double qOldCellLocal  = static_cast<double>(prevQBytes) / 600.0;

        // 3. 调用 FRP 计算器。本轮决策使用"上一轮累计"的计数器。
        double calculatedRateBps = m_frpCalculator.CalculateFairRate(
            ifIndex,
            link_bps,
            currentQDepth,
            ccMode - 13,
            ccMode,
            static_cast<uint8_t>(crossDcCounter > 255 ? 255 : crossDcCounter)
        );

        // 4. 更新计数器：两个条件同时成立才累加，任一不满足则重置
        if (qCurCellLocal > qOldCellLocal && qCurCellLocal > (qRefCellLocal + qThCellLocal)) {
            m_crossDcCounter[ifIndex] = (crossDcCounter < CROSS_DC_CONSEC_LIMIT + 5)
                                         ? crossDcCounter + 1 : crossDcCounter;
        } else {
            m_crossDcCounter[ifIndex] = 0;
        }
        // 5. 保存本周期队列深度，供下周期对比
        m_prevQBytes[ifIndex] = currentQDepth;

        // 2. 严格遵循控制包字段的局部量纲换算
        uint16_t fairRateField = static_cast<uint16_t>(calculatedRateBps / 10000000.0);  // 换算为 10Mbps 单元
        double qRefBytes = qRefBytesLocal;
        double qCurCell = qCurCellLocal;
        double qRefCell = qRefCellLocal;
        double qDevCell = qCurCell - qRefCell;
        int16_t qDevField   = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, qDevCell)));  // 换算为 600B Cell 单元 (可为负值)
        uint16_t linkRateField = static_cast<uint16_t>(link_bps / 10000000.0);  // 换算为 10Mbps 单元

        // Current topologies use fewer than 256 nodes and 256 ports per switch.
        // Keep both components collision-free within the 16-bit CP field.
        uint16_t cpId = static_cast<uint16_t>(((m_id & 0xFF) << 8) |
                                              (ifIndex & 0xFF));

        // 调试：输出linkRate计算
        std::cout << "  [FRP CALC] Switch " << m_id << " port=" << ifIndex
                  << " link_bps=" << link_bps << " linkRateField=" << linkRateField
                  << " fairRateField=" << fairRateField << " qDevField=" << qDevField
                  << " (qCur=" << qCurCell << " qRef=" << qRefCell << ")"
                  << " type=" << ccMode << std::endl;

        // 结构化数据日志。与 TX RATE 和 DCQCN 队列日志共用 stdout，避免
        // runner 合并 stdout/stderr 时发生记录交叉。
        double fairRateMbps = calculatedRateBps / 1e6;
        double qCurKB = static_cast<double>(currentQDepth) / 1024.0;
        printf("[FRP_DATA_SW] %.9f %u %u %.2f %.2f %.2f %d\n",
                Simulator::Now().GetSeconds(), m_id, ifIndex,
                fairRateMbps, qCurKB, (qDevField * 600.0 / 1024.0), ccMode);

        // 3. 装载并打包
        Icmpv4FrpFeedback frpHeader;
        frpHeader.SetFairRate(fairRateField);
        frpHeader.SetQDepth(qDevField);
        frpHeader.SetCpId(cpId);
        frpHeader.SetType(ccMode - 13);
        frpHeader.SetLinkRate(linkRateField);  // 把带宽塞进 15bit 空间传出
        payload->AddHeader(frpHeader);

        // 4. 广域流标志已随周期清空，无需单独清除

        // 5. 添加 ICMP 头部
        Icmpv4Header icmpHeader;
        icmpHeader.SetType(Icmpv4Header::ICMPV4_FRP_FEEDBACK);
        icmpHeader.SetCode(0);
        icmpHeader.EnableChecksum();
        payload->AddHeader(icmpHeader);

        NS_LOG_DEBUG("FRP Encap -> FairRate: " << fairRateField << "*10Mbps, "
                     << "LinkRate: " << linkRateField << "*10Mbps, "
                     << "QDev: " << qDevField << " cells(600B)");
    }
    else if (ccMode == 15) {
        // ========== 未来扩展：其他自定义模式 ==========
        NS_LOG_DEBUG("Custom mode " << ccMode << " not implemented");
    }
    else {
        NS_LOG_WARN("Unknown ccMode=" << ccMode << ", no feedback payload generated");
    }
    
    return payload;
}

/**
 * 核心任务函数 2：通用的网络层/链路层封装与发送
 * 作用：使用仿真平台的CustomHeader封装FRP包并送入发送引擎
 * 
 * 包格式：[CustomHeader(L2+L3, l3Prot=0x01)] [Icmpv4FrpFeedback payload]
 * CustomHeader内部已包含PPP+IPv4信息，不需要额外添加PppHeader/Ipv4Header
 */
void SwitchNode::SendControlPacket(Ipv4Address srcAddr,
                                    Ipv4Address dstAddr,
                                    Ptr<Packet> payload,
                                    uint16_t flowSport,
                                    uint16_t flowPg) {
    Ptr<Packet> p = payload->Copy();

    Icmpv4Header icmpHeader;
    Icmpv4FrpFeedback feedbackHeader;
    p->RemoveHeader(icmpHeader);
    p->RemoveHeader(feedbackHeader);
    feedbackHeader.SetFlowSport(flowSport);
    feedbackHeader.SetFlowPg(flowPg);
    p->AddHeader(feedbackHeader);
    p->AddHeader(icmpHeader);

    // 构造CustomHeader (仿真平台的标准封装)
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header);
    ch.l3Prot = 0x01;  // ICMP - 让主机端路由到ReceiveIcmp

    // L2: PPP
    ch.pppProto = 0x0021;  // IPv4

    // L3: IPv4
    ch.sip = srcAddr.Get();
    ch.dip = dstAddr.Get();
    ch.m_ttl = 64;
    ch.ipid = rand() % 65536;

    // 设置payload大小
    ch.m_payloadSize = p->GetSize();

    // 将CustomHeader封装到packet前面
    p->AddHeader(ch);

    // 用MyPriorityTag标记为最高优先级(0)，避免被低优先级队列延迟
    MyPriorityTag prioTag;
    prioTag.SetPriority(0);  // queue 0 = highest priority
    p->AddPacketTag(prioTag);

    // 设置入端口标签，标识该包由交换机内部产生
    InterfaceTag intTag(m_id);
    p->AddPacketTag(intTag);

    std::cout << "  [FRP SEND] SendToDev: src=" << srcAddr << " dst=" << dstAddr
              << " pktSize=" << p->GetSize() << " ch.l3Prot=0x" << std::hex << (int)ch.l3Prot << std::dec << std::endl;

    // 选择出端口：仅根据 DIP 查路由表
    uint32_t outPort = (uint32_t)-1;
    auto entry = m_rtTable.find(dstAddr.Get());
    if (entry != m_rtTable.end() && !entry->second.empty()) {
        outPort = entry->second[0];  // 取第一个可用端口
        std::cout << "  [FRP SEND] Route-table outPort=" << outPort
                  << " (dst=" << dstAddr << ")" << std::endl;
    }

    if (outPort != (uint32_t)-1 && outPort < m_devices.size()) {
        m_devices[outPort]->SwitchSend(0, p, ch);  // qIndex=0, 最高优先级
        std::cout << "  [FRP SEND] Sent via port " << outPort << std::endl;
    } else {
        std::cout << "  [FRP SEND] ERROR: No valid outPort for dst=" << dstAddr << std::endl;
    }
}

// 调试接口：打印指向特定目的 IP 的路由表条目
void SwitchNode::PrintRoutingTableFor(Ipv4Address targetDst) {
    std::cout << "  [RTABLE] Switch " << m_id << " routes for dst=" << targetDst << " (0x"
              << std::hex << targetDst.Get() << std::dec << "):" << std::endl;
    auto entry = m_rtTable.find(targetDst.Get());
    if (entry == m_rtTable.end()) {
        std::cout << "    (no entry)" << std::endl;
        return;
    }
    std::cout << "    ports=[";
    for (size_t i = 0; i < entry->second.size(); i++) {
        if (i > 0) std::cout << ",";
        std::cout << entry->second[i];
    }
    std::cout << "]" << std::endl;
}

/**
 * 设置交换机真实IP地址
 * 用于FRP反馈包的源IP地址
 */
void SwitchNode::SetSwitchRealIp(Ipv4Address ip) {
    m_switchRealIp = ip;
    NS_LOG_INFO("Switch " << m_id << " real IP set to " << ip);
}

/**
 * 获取交换机真实IP地址
 */
Ipv4Address SwitchNode::GetSwitchRealIp() const {
    return m_switchRealIp;
}

// ========== Bifrost (ccMode=12) 实现 ==========

/**
 * 启动Bifrost周期PFC机制
 * 仅在启用了Bifrost的交换机（通常是指定的某个交换机）上调用
 */
void SwitchNode::ConfigureBifrost(uint32_t port, uint32_t priority, uint64_t bufferBytes, uint32_t leakSlots) {
    NS_ABORT_MSG_IF(port == 0 || port >= m_devices.size() || priority == 0 || priority >= qCnt ||
                    leakSlots == 0 || m_bifrostPfcPeriodUs == 0, "Invalid Bifrost configuration");
    m_bifrostIngress = port;
    m_bifrostPriority = priority;
    m_bifrostBuffer = bufferBytes;
    m_bifrostLeakSlots = leakSlots;
    auto dev = DynamicCast<QbbNetDevice>(m_devices[port]);
    double rs = dev->GetDataRate().GetBitRate() / 8.0;
    double t = m_bifrostPfcPeriodUs * 1e-6;
    double rtt = 2 * DynamicCast<QbbChannel>(dev->GetChannel())->GetDelay().GetSeconds();
    m_bifrostBound = rs * (rtt + t); // paper Delta uses link RTT, not one-way delay
    NS_ABORT_MSG_IF(bufferBytes < rs * (rtt + 2*t), "Bifrost requires H >= Delta + 2 Rs T");
    m_bifrostVirtual = m_bifrostBound;
}

void SwitchNode::StartBifrostPfcMechanism() {
    if (!m_bifrostEnabled) return;
    NS_ABORT_MSG_IF(m_bifrostIngress == 0, "Configure Bifrost before starting it");
    m_bifrostPfcEvent = Simulator::Schedule(MicroSeconds(m_bifrostPfcPeriodUs),
        &SwitchNode::SendBifrostPfcWithCustomTime, this);
}

void SwitchNode::SendBifrostPfcWithCustomTime() {
    if (!m_bifrostEnabled) return;
    auto dev = DynamicCast<QbbNetDevice>(m_devices[m_bifrostIngress]);
    uint64_t received = m_bifrostReceived - m_bifrostLastReceived;
    m_bifrostLastReceived = m_bifrostReceived;
    uint32_t queued = 0;
    for (uint32_t out = 1; out < m_devices.size(); ++out)
        queued += m_bytes[m_bifrostIngress][out][m_bifrostPriority];
    uint32_t pause = CalculateBifrostPauseTime(m_bifrostPriority, queued, received);
    if (pause > 0) dev->SendPfcWithTime(m_bifrostPriority, 0, pause);
    // One compact record per control slot, also useful for smoke-test validation.
    std::cout << "[BIFROST_STATE] " << Simulator::Now().GetNanoSeconds() << " "
        << m_id << " " << m_bifrostIngress << " " << queued << " " << received
        << " " << m_bifrostVirtual << " " << pause << std::endl;
    m_bifrostPfcEvent = Simulator::Schedule(MicroSeconds(m_bifrostPfcPeriodUs),
        &SwitchNode::SendBifrostPfcWithCustomTime, this);
}

uint32_t SwitchNode::CalculateBifrostPauseTime(uint32_t, uint32_t queued, uint64_t received) {
    auto dev = DynamicCast<QbbNetDevice>(m_devices[m_bifrostIngress]);
    double rs = dev->GetDataRate().GetBitRate() / 8.0;
    double slotBytes = rs * m_bifrostPfcPeriodUs * 1e-6;
    double grant = std::min(slotBytes, double(m_bifrostBuffer) - queued - m_bifrostVirtual);
    double corrected = grant;
    if (++m_bifrostSlot % m_bifrostLeakSlots == 0)
        corrected = std::max(0.0, grant - (queued + m_bifrostVirtual - m_bifrostBuffer));
    // Preserve signed grant in virtual-state correction; physical pause is bounded.
    corrected = std::max(0.0, std::min(slotBytes, corrected));
    double pause = m_bifrostPfcPeriodUs - corrected / rs * 1e6;
    m_bifrostVirtual = std::max(0.0, std::min(m_bifrostBound, m_bifrostVirtual - received + grant));
    return std::min(m_bifrostPfcPeriodUs, uint32_t(std::ceil(std::max(0.0, pause))));
}

} // namespace ns3
