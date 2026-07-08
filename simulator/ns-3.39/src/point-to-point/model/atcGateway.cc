#include "ns3/atcGateway.h"
#include "ns3/packet.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-header.h"
#include "ns3/tcp-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/random-variable.h"
#include "point-to-point-net-device.h"
#include "ns3/custom-priority-tag.h"
#include "ns3/interface-tag.h"
#include <assert.h>
#include <sys/time.h>

namespace ns3{

atcGateway::atcGateway()
{
    m_debug = 0x3;
    m_gatewayStatus = 0;
    m_gatewayType = 0;
    m_congested = false;
    m_ecnbits = 0;
    m_packet_size = 0; // 报文净荷大小
    m_voqMaxSize = VOQ_MAX_SIZE;
    m_cnpNotifyInterval = CNP_NOTIFY_INTERVAL;
    m_cnpOnGlobalCongestion = true;
    m_voqReactToCnp = true;
    m_cnpByGlobalCongestion = 0;
    m_cnpByPacketEcn = 0;

     /******************************
	 * Mellanox's version of DCQCN
	 *****************************/
    m_g=0.00390625; //feedback weight
    m_alpha_resume_interval = 1.0;
    m_rateDecreaseInterval = 4.0;
    m_rpgTimeReset = 480.0;
    m_rateOnFirstCNP = 1.0;
    m_rpgThreshold = 1;
    m_EcnClampTgtRate = false;
    m_minRate = DataRate("100Mb/s");
    m_rai = DataRate("50Mb/s");	
	m_rhai = DataRate("100Mb/s");
}

atcGateway::~atcGateway(){}

void atcGateway::ConfigureAtcParams(uint32_t voqMaxSize,
                                    double cnpNotifyIntervalUs,
                                    const std::string& minRate,
                                    double rateDecreaseIntervalUs,
                                    double rpgTimeResetUs,
                                    bool cnpOnGlobalCongestion,
                                    bool voqReactToCnp)
{
    m_voqMaxSize = voqMaxSize;
    m_cnpNotifyInterval = MicroSeconds(cnpNotifyIntervalUs);
    m_minRate = DataRate(minRate);
    m_rateDecreaseInterval = rateDecreaseIntervalUs;
    m_rpgTimeReset = rpgTimeResetUs;
    m_cnpOnGlobalCongestion = cnpOnGlobalCongestion;
    m_voqReactToCnp = voqReactToCnp;

    if (m_debug & 0x2) {
        printf("+++atcGateway params voqMaxSize:%u cnpNotifyInterval:%.3fus minRate:%s rateDecreaseInterval:%.3fus rpgTimeReset:%.3fus cnpOnGlobal:%u voqReactToCnp:%u\n",
               m_voqMaxSize, cnpNotifyIntervalUs, minRate.c_str(), m_rateDecreaseInterval, m_rpgTimeReset,
               m_cnpOnGlobalCongestion ? 1 : 0, m_voqReactToCnp ? 1 : 0);
    }
}

void atcGateway::init(int type,uint32_t max_voq_count,uint64_t max_voq_rate, uint64_t longHaulBandwidth,Time longHaulDelay)
{
    m_gatewayStatus = 1;
    m_gatewayType = type;
    m_maxVoqCount = max_voq_count;
    m_maxVoqRate = max_voq_rate;

    // 初始化VOQ池
    voqPool.resize(m_maxVoqCount);
    for (uint32_t i = 0; i < m_maxVoqCount; i++) {
        voqPool[i].voqId = i;
        voqPool[i].currentSize = 0;
        voqPool[i].maxSize = m_voqMaxSize;
        voqPool[i].currentInflightBytes = 0;
        voqPool[i].maxInflightBytes =  m_maxVoqRate * INTRA_DC_DELAY.GetSeconds() *2/8.0;
        voqPool[i].isAllocated = false;
        voqPool[i].lastTransmitTime = Seconds(0);
    }
    
    InitLongHaulState(longHaulBandwidth,longHaulDelay);
}

void atcGateway::setSwitchSendToDevCallback(Callback<void, Ptr<Packet>, CustomHeader&> cb)
{
    m_sendCallBack = cb;
}

void atcGateway::DoSwitchSendToDev(Ptr<Packet> p, CustomHeader& ch)
{
    m_sendCallBack(p, ch);
}

void atcGateway::setGetOutDevCallback(Callback<Ptr<QbbNetDevice>, Ptr<Packet>, CustomHeader&> cb)
{
    m_getOutDevCallBack = cb;
}

Ptr<QbbNetDevice> atcGateway::GetOutDevice(Ptr<Packet> p, CustomHeader& ch)
{
    return m_getOutDevCallBack(p, ch);
}

uint16_t atcGateway::EtherToPpp (uint16_t proto) {
	switch (proto) {
	case 0x0800: return 0x0021;   //IPv4
	case 0x86DD: return 0x0057;   //IPv6
	default: NS_ASSERT_MSG (false, "PPP Protocol number not defined!");
	}
	return 0;
}

// ========== VOQ管理函数 ==========

// 分配VOQ
uint32_t atcGateway::AllocateVoq(Ipv4Address destIp) {
    // 检查是否已分配
    auto it = destToVoqMap.find(destIp);
    if (it != destToVoqMap.end()) {
        return it->second;
    }
    
    // 查找空闲VOQ
    for (uint32_t i = 0; i < m_maxVoqCount; i++) {
        if (!voqPool[i].isAllocated) {
            voqPool[i].isAllocated = true;
            voqPool[i].destIp = destIp;
            voqPool[i].dcqcn_initialized = true;
            voqPool[i].m_rate = DataRate(m_maxVoqRate);
            voqPool[i].mlx.m_targetRate = DataRate(m_maxVoqRate);
            
            destToVoqMap[destIp] = i;
            longHaulState.activeVoqCount++;
            
            if (m_debug & 0x2) {
                printf("+++atcGateway AllocateVoq voqId:%d destIp:%d  time:%.9f\n", 
                       i, destIp.Get(),  Simulator::Now().GetSeconds());
            }
            
            return i;
        }
    }
    
    // 如果没有空闲VOQ
    return -1;
}

// 释放VOQ
void atcGateway::ReleaseVoq(uint32_t voqId) {
    if (voqId >= voqPool.size()) return;
    
    VoqEntry& voq = voqPool[voqId];
    
    if (voq.isAllocated) {
        // 取消任何待处理的转发事件
        if (voq.transmitEvent.IsRunning()) {
            Simulator::Cancel(voq.transmitEvent);
        }
        
        // 清理队列
        while (!voq.packetQueue.empty()) {
            voq.packetQueue.pop();
        }
        
        // 重置VOQ状态
        voq.isAllocated = false;
        voq.currentSize = 0;
        voq.lastTransmitTime = Seconds(0);
        
        // 从映射中移除
        destToVoqMap.erase(voq.destIp);
        longHaulState.activeVoqCount--;
        
        voq.destIp = Ipv4Address();

        if (m_debug & 0x2) {
                printf("+++atcGateway ReleaseVoq voqId:%d  time:%.9f\n", 
                       voqId,   Simulator::Now().GetSeconds());
        }
    }
}

// VOQ统计
void atcGateway::VoqStat() {
    uint64_t maxTotalVoqBytes = 0;
    // 查找空闲VOQ
    for (uint32_t i = 0; i < m_maxVoqCount; i++) {
        if (voqPool[i].isAllocated) {
            if (voqPool[i].currentSize > longHaulState.maxVoqBytes) {
                longHaulState.maxVoqBytes = voqPool[i].currentSize;
            }
            maxTotalVoqBytes += voqPool[i].currentSize;
        }
    }

    if (maxTotalVoqBytes > longHaulState.maxTotalVoqBytes) {
        longHaulState.maxTotalVoqBytes = maxTotalVoqBytes;
    }
    return ;
}

// 计算数据包传输时间
Time atcGateway::CalculateTransmitTime(uint32_t pktSize, DataRate rate) {
    if (rate.GetBitRate() == 0) {
        return Seconds(0);
    }
    
    uint64_t packetSizeBits = pktSize * 8;
    double transmitTimeSeconds = static_cast<double>(packetSizeBits) / rate.GetBitRate();
    
    return Seconds(transmitTimeSeconds);
}

// 从VOQ转发数据包
void atcGateway::TransmitFromVoq(uint32_t voqId) {
    if (voqId >= voqPool.size()) return;
    
    VoqEntry& voq = voqPool[voqId];

    if (!voq.isAllocated) return;
    
    if (voq.packetQueue.empty()) {
        // 如果队列变空，考虑释放VOQ
        if (!voq.voqReleaseEvent.IsRunning()) {
            voq.voqReleaseEvent = Simulator::Schedule(Seconds(0.001), &atcGateway::CheckVoqRelease, this, voqId);
        }
        return;
    }
    
    // 取出队列头部的数据包
    Ptr<Packet> pkt = voq.packetQueue.front();
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    pkt->PeekHeader(ch);
    uint32_t pktSize = pkt->GetSize();

    // 限制转发字节数在下游DC 的bdp内
    if (voq.currentInflightBytes < voq.maxInflightBytes) 
    {
        voq.packetQueue.pop();
        voq.currentSize -= pktSize;

        if (longHaulState.longHaulSrcIps.find(ch.sip) != longHaulState.longHaulSrcIps.end()) {
            longHaulState.remoteForwardedBytes += pktSize;
        }
        
        // 转发数据包
        DoSwitchSendToDev(pkt, ch);

        // 报文统计
        voq.currentInflightBytes += pktSize;
    }
    
    // 更新最后转发时间
    voq.lastTransmitTime = Simulator::Now();
    
    // 计算下一个数据包的转发时间
    Time transmitTime = CalculateTransmitTime(pktSize, voq.m_rate);
    /*if ( Simulator::Now().GetSeconds() >= 0.0061 && Simulator::Now().GetSeconds() <= 0.0071 ) {
        printf("+++atcGateway TransmitFromVoq voqId:%d rate:%.3f sendInterval:%lu time:%.9f\n", 
            voqId, voq->m_rate.GetBitRate() * 1e-9, transmitTime.GetTimeStep(), Simulator::Now().GetSeconds());
    }*/
    
    // 调度下一次转发
    voq.transmitEvent = Simulator::Schedule(transmitTime, 
                                            &atcGateway::TransmitFromVoq, 
                                            this, voqId);
    
    /*if (m_debug & 0x2) {
        printf("+++atcGateway TransmitFromVoq voqId:%d destIp:%d queueSize:%lu time:%.9f\n", 
               voqId, voq.destIp.Get(), voq.currentSize, Simulator::Now().GetSeconds());
    }*/
}

// 检查VOQ是否可以释放
void atcGateway::CheckVoqRelease(uint32_t voqId) {
    if (voqId >= voqPool.size()) return;
    
    VoqEntry& voq = voqPool[voqId];
    
    // 如果队列仍然为空且没有正在运行的转发事件，释放VOQ
    if (voq.packetQueue.empty() && !voq.transmitEvent.IsRunning()) {
        ReleaseVoq(voqId);
    }
}

// 调度VOQ转发
void atcGateway::ScheduleVoqForward(uint32_t voqId) {
    if (voqId >= voqPool.size()) return;
    
    VoqEntry& voq = voqPool[voqId];
    if (voq.packetQueue.empty()) {
        return;
    }
    
    // 如果已经有转发事件在运行，不重复调度
    if (voq.transmitEvent.IsRunning()) {
        return;
    }
    
    // 立即开始转发第一个数据包
    TransmitFromVoq(voqId);
}

// 获取或创建调度表项
ScheduleEntry atcGateway::GetOrCreateScheduleEntry(Ipv4Address destIp) {
    auto it = scheduleTable.find(destIp);
    if (it != scheduleTable.end()) {
        return it->second;
    }
    
    // 创建新的调度表项
    ScheduleEntry entry;
    entry.destIp = destIp;
    entry.rateLimit = m_maxVoqRate; 
    entry.isCongested = false;
    entry.lastUpdateTime = Simulator::Now();
    scheduleTable[destIp] = entry;
    return entry;
}


void atcGateway::sendCnp(CustomHeader &ch, Ptr<NetDevice> ingressDev)
{
    qbbHeader seqh;
    seqh.SetSeq(0);
    seqh.SetPG(ch.udp.pg);
    seqh.SetSport(ch.udp.dport);
    seqh.SetDport(ch.udp.sport);
    seqh.SetIntHeader(ch.udp.ih);
	seqh.SetCnp();

    Ptr<Packet> newp = Create<Packet>(std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
    newp->AddHeader(seqh);

    Ipv4Header head;
    head.SetDestination(Ipv4Address(ch.sip));
    head.SetSource(Ipv4Address(ch.dip));
    head.SetProtocol(0xFC);
    head.SetTtl(64);
    head.SetPayloadSize(newp->GetSize());
    head.SetIdentification(UniformVariable(0, 65536).GetValue());
    newp->AddHeader(head);

    PppHeader ppp;
    ppp.SetProtocol (EtherToPpp (0x800));
    newp->AddHeader (ppp);

    MyPriorityTag pri;
    pri.SetPriority(0);
    newp->AddPacketTag(pri);
    if (ingressDev != nullptr) {
        newp->AddPacketTag(InterfaceTag(ingressDev->GetIfIndex()));
    }

    CustomHeader bfCh(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    newp->PeekHeader(bfCh);
    DoSwitchSendToDev(newp, bfCh);

    if (m_debug & 0x1)
        printf("+++atcGateway sendCnp  time:%f\n",Simulator::Now().GetSeconds());
}

/******************************
 * Mellanox's version of DCQCN (VOQ版本)
 *****************************/
void atcGateway::UpdateAlphaMlxVoq(VoqEntry *voq) {
	if (voq->mlx.m_alpha_cnp_arrived) {
		voq->mlx.m_alpha = (1 - m_g) * voq->mlx.m_alpha + m_g;
	} else {
		voq->mlx.m_alpha = (1 - m_g) * voq->mlx.m_alpha;
	}
	voq->mlx.m_alpha_cnp_arrived = false;
	ScheduleUpdateAlphaMlxVoq(voq);
}

void atcGateway::ScheduleUpdateAlphaMlxVoq(VoqEntry *voq) {
	voq->mlx.m_eventUpdateAlpha = Simulator::Schedule(MicroSeconds(m_alpha_resume_interval), &atcGateway::UpdateAlphaMlxVoq, this, voq);
}

void atcGateway::cnp_received_mlx_voq(VoqEntry *voq) {
	voq->mlx.m_alpha_cnp_arrived = true;
	voq->mlx.m_decrease_cnp_arrived = true;
	if (voq->mlx.m_first_cnp) {
		voq->mlx.m_alpha = 1;
		voq->mlx.m_alpha_cnp_arrived = false;
		ScheduleUpdateAlphaMlxVoq(voq);
		ScheduleDecreaseRateMlxVoq(voq, 1);
		voq->mlx.m_targetRate = voq->m_rate = m_rateOnFirstCNP * voq->m_rate;
		voq->mlx.m_first_cnp = false;
	}
}

void atcGateway::CheckRateDecreaseMlxVoq(VoqEntry *voq) {
	ScheduleDecreaseRateMlxVoq(voq, 0);
	if (voq->mlx.m_decrease_cnp_arrived) {
#if PRINT_LOG
		printf("+++atcGateway %lu rate dec: (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
		bool clamp = true;
		if (!m_EcnClampTgtRate) {
			if (voq->mlx.m_rpTimeStage == 0)
				clamp = false;
		}
		if (clamp)
			voq->mlx.m_targetRate = voq->m_rate;
		voq->m_rate = std::max(m_minRate, voq->m_rate * (1 - voq->mlx.m_alpha / 2));
		voq->mlx.m_rpTimeStage = 0;
		voq->mlx.m_decrease_cnp_arrived = false;
		Simulator::Cancel(voq->mlx.m_rpTimer);
		voq->mlx.m_rpTimer = Simulator::Schedule(MicroSeconds(m_rpgTimeReset), &atcGateway::RateIncEventTimerMlxVoq, this, voq);
#if PRINT_LOG
		printf("(%.3lf %.3lf)\n", voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
	}
}

void atcGateway::ScheduleDecreaseRateMlxVoq(VoqEntry *voq, uint32_t delta) {
	voq->mlx.m_eventDecreaseRate = Simulator::Schedule(MicroSeconds(m_rateDecreaseInterval) + NanoSeconds(delta), &atcGateway::CheckRateDecreaseMlxVoq, this, voq);
}

void atcGateway::RateIncEventTimerMlxVoq(VoqEntry *voq) {
	voq->mlx.m_rpTimer = Simulator::Schedule(MicroSeconds(m_rpgTimeReset), &atcGateway::RateIncEventTimerMlxVoq, this, voq);
	RateIncEventMlxVoq(voq);
	voq->mlx.m_rpTimeStage++;
}

void atcGateway::RateIncEventMlxVoq(VoqEntry *voq) {
	if (voq->mlx.m_rpTimeStage < m_rpgThreshold) {
		FastRecoveryMlxVoq(voq);
	} else if (voq->mlx.m_rpTimeStage == m_rpgThreshold) {
		ActiveIncreaseMlxVoq(voq);
	} else {
		HyperIncreaseMlxVoq(voq);
	}
}

void atcGateway::FastRecoveryMlxVoq(VoqEntry *voq) {
#if PRINT_LOG
	printf("+++atcGateway %lu fast recovery: (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
	voq->m_rate = (voq->m_rate / 2) + (voq->mlx.m_targetRate / 2);

#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
}

void atcGateway::ActiveIncreaseMlxVoq(VoqEntry *voq) {
#if PRINT_LOG
	printf("+++atcGateway %lu active inc:  (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
	voq->mlx.m_targetRate += m_rai;
	if (voq->mlx.m_targetRate > DataRate(m_maxVoqRate))
		voq->mlx.m_targetRate = DataRate(m_maxVoqRate);
	voq->m_rate = (voq->m_rate / 2) + (voq->mlx.m_targetRate / 2);
#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
}

void atcGateway::HyperIncreaseMlxVoq(VoqEntry *voq) {
#if PRINT_LOG
	printf("+++atcGateway %lu hyper inc:(%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
	voq->mlx.m_targetRate += m_rhai;
	if (voq->mlx.m_targetRate > DataRate(m_maxVoqRate))
		voq->mlx.m_targetRate = DataRate(m_maxVoqRate);
    if ((voq->mlx.m_rpTimeStage >= 5) &&  voq->mlx.m_targetRate <  DataRate(m_maxVoqRate))
		voq->mlx.m_targetRate =  DataRate(m_maxVoqRate);
	voq->m_rate = (voq->m_rate / 2) + (voq->mlx.m_targetRate / 2);
#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
}

// ========== 主要的packetIn函数（VOQ版本） ==========
bool atcGateway::packetIn(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch)
{
    if (m_gatewayStatus != 1) {
        return false;
    }

    if (m_gatewayType == 1) { // 发送端门廊
        if (ch.l3Prot == 0x11) { // RDMA UDP数据报文
            m_ecnbits = ch.GetIpv4EcnBits();
            bool triggerByPacketEcn = (m_ecnbits != 0);
            bool triggerByGlobalCongestion = (m_cnpOnGlobalCongestion && m_congested);
            if (triggerByPacketEcn || triggerByGlobalCongestion) {
                if (!(cnpNotifyTime.count(ch.sip) > 0 && (Simulator::Now() - cnpNotifyTime[ch.sip]) < m_cnpNotifyInterval)) {
                    cnpNotifyTime[ch.sip] = Simulator::Now();
                    if (triggerByPacketEcn) {
                        m_cnpByPacketEcn++;
                    } else {
                        m_cnpByGlobalCongestion++;
                    }
                    sendCnp(ch, device); // 回复CNP
                    uint64_t cnpTotal = m_cnpByPacketEcn + m_cnpByGlobalCongestion;
                    if ((m_debug & 0x2) && (cnpTotal <= 20 || cnpTotal % 500 == 0)) {
                        printf("+++atcGateway CNP reason total:%lu packetEcn:%lu globalCong:%lu lastEcn:%u globalState:%u time:%f\n",
                               cnpTotal, m_cnpByPacketEcn, m_cnpByGlobalCongestion,
                               m_ecnbits, m_congested ? 1 : 0, Simulator::Now().GetSeconds());
                    }
                }
            }
            longHaulState.localSentBytes += pkt->GetSize();
            return false;
        }
        else if (ch.l3Prot == 0x6) { // LongHaulFeedback
            return HandleLongHaulFeedback(device, pkt, ch);
        }
    }
    else if (m_gatewayType == 2) { // 接收端门廊（VOQ模式）
        if (ch.l3Prot == 0x11) { // RDMA UDP数据报文
            return HandleDataPacketWithVoq(device, pkt, ch);
        }
        else if (ch.l3Prot == 0xFC) { // RDMA ACK报文
            return HandleAckPacketWithVoq(device, pkt, ch);
        }
    }

    return false;
}

// 使用VOQ处理数据包
bool atcGateway::HandleDataPacketWithVoq(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch) {

    // 提取目的IP
    Ipv4Address destIp = Ipv4Address(ch.dip);
    
    // 创建清理ECN标记的数据包副本
    Ptr<Packet> cleanPkt = pkt->Copy();
    if (ch.GetIpv4EcnBits()) { // 清除ECN标记
        PppHeader ppp;
        Ipv4Header h;
        cleanPkt->RemoveHeader(ppp);
        cleanPkt->RemoveHeader(h);
        h.SetEcn((Ipv4Header::EcnType)0x0);
        cleanPkt->AddHeader(h);
        cleanPkt->AddHeader(ppp);
        ch.m_tos &= 0xFC;
    }



        Ptr<QbbNetDevice> qbbDevice = DynamicCast<QbbNetDevice>(device);
        Ptr<QbbChannel> qbbChannel;
        if (qbbDevice) {
            qbbChannel = DynamicCast<QbbChannel>(qbbDevice->GetChannel());
        }

        if (qbbChannel && qbbChannel->GetDelay().GetTimeStep() > 100000) // 远端的报文
        {
        if (!longHaulState.longHaulChFlag) {
            longHaulState.longHaulCh = ch;
            longHaulState.longHaulChFlag = true;
            StartLongHaulFeedbackTimer();
        }
        longHaulState.longHaulIngressIfIndex = qbbDevice->GetIfIndex();

        // 记录远端源IP地址
        if (longHaulState.longHaulSrcIps.find(ch.sip) == longHaulState.longHaulSrcIps.end()) {
            longHaulState.longHaulSrcIps.insert(ch.sip);
        }
    }
    
    // 使用VOQ调度
    int32_t voqId = AllocateVoq(destIp);
    if (voqId < 0) {
        // 没有可用VOQ，直接转发清理后的数据包
        DoSwitchSendToDev(cleanPkt, ch);
        return true; // 拦截原始数据包
    }
    
    VoqEntry& voq = voqPool[voqId];
    
    // 将清理后的数据包存储到VOQ
    voq.packetQueue.push(cleanPkt);
    voq.currentSize += cleanPkt->GetSize();
    VoqStat();

    if (m_packet_size == 0) {
        m_packet_size = cleanPkt->GetSize();
    }
    
    // 触发VOQ转发（使用VOQ自身的速率）
    if (!voq.transmitEvent.IsRunning()) {
        ScheduleVoqForward(voqId);
    }
    
    /*if (m_debug & 0x2) {
        printf("+++atcGateway VOQ store destIp:%d voqId:%d queueSize:%lu  time:%.9f\n", 
               destIp.Get(), voqId, voq.currentSize,  Simulator::Now().GetSeconds());
    }*/
    
    return true; // 拦截原始数据包
}

// 使用VOQ处理ACK包
bool atcGateway::HandleAckPacketWithVoq(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch) {
    uint8_t cnp = (ch.ack.flags >> qbbHeader::FLAG_CNP) & 1;
    
    // 提取目的IP（对于ACK包，目的IP是原始数据包的源IP）
    Ipv4Address destIp = Ipv4Address(ch.sip);
    
    // 查找对应的VOQ
    auto voqIt = destToVoqMap.find(destIp);
    if (voqIt != destToVoqMap.end()) {
        uint32_t voqId = voqIt->second;
        VoqEntry& voq = voqPool[voqId];

        if (voq.currentInflightBytes >= m_packet_size) {
            voq.currentInflightBytes -= m_packet_size;
        }
        else {
            voq.currentInflightBytes = 0;
        }
        
        // 处理CNP
        if (cnp) {
            // 使用VOQ版本的DCQCN处理CNP
            if (m_voqReactToCnp) {
                cnp_received_mlx_voq(&voq);
            }
            
            if (m_debug & 0x2) {
                printf("+++atcGateway CNP received for VOQ voqId:%d destIp:%d newRate:%lu time:%.9f\n", 
                       voqId, destIp.Get(), voq.m_rate.GetBitRate(), Simulator::Now().GetSeconds());
            }
            
            return false; // 转发携带CNP标记的ACK报文给上游
        }
    }
    
    return false; // 转发携带CNP标记的ACK报文给上游
}

// 限流本地转发速率
void atcGateway::ThrottleLocalForwarding(Ptr<NetDevice> dev, uint64_t rate) {
    if (!dev) return;
    
    Ptr<QbbNetDevice> qbbDev = DynamicCast<QbbNetDevice>(dev);
    if (qbbDev) {
        // 使用QBB设备的速率控制
        qbbDev->SetDataRate(DataRate(rate));
    }
}

// 处理LongHaulFeedback
bool atcGateway::HandleLongHaulFeedback(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch) {

    uint64_t remoteForwardedBytes =  (((uint64_t)ch.tcp.ack) << 32) | (uint64_t)ch.tcp.seq;
    uint32_t activeVoqCount = ch.tcp.dport;
    uint64_t rateLimit = 0;
    bool limit = false;

    if (m_debug & 0x2)
         printf("+++atcGateway recv LongHaulFeedback localSentBytes:%lu remoteForwardedBytes:%lu  inflightBytes:%lu activeVoqCount:%d  time:%f\n",
            longHaulState.localSentBytes,remoteForwardedBytes,longHaulState.localSentBytes - remoteForwardedBytes,activeVoqCount,Simulator::Now().GetSeconds());

    if (longHaulState.localSentBytes > remoteForwardedBytes) {
        uint64_t inflightBytes = longHaulState.localSentBytes - remoteForwardedBytes;
        
        // 若在途字节数超过阈值，限流
        if (inflightBytes > longHaulState.maxInflightBytes) {
            
            if (longHaulState.inflightBytes > 0 && longHaulState.lastLocalSentBytes > 0 && inflightBytes > longHaulState.inflightBytes && longHaulState.localSentBytes > longHaulState.lastLocalSentBytes)
            {
                double ratio = 0.9;
                ratio = (double) (inflightBytes - longHaulState.inflightBytes) / (longHaulState.localSentBytes - longHaulState.lastLocalSentBytes);
                if (ratio >= 0.05 && ratio <= 0.95)
                {
                    double K = 1.0 / m_maxVoqCount;
                    ratio = 1.0 - ratio;
                    rateLimit = ratio*longHaulState.longHaulBandwidth;
                    if ((rateLimit != longHaulState.rateLimit) && (ratio >= K)) //波动才限流
                    {
                        ThrottleLocalForwarding(device, rateLimit); // 限流
                    }
                    longHaulState.rateLimit = rateLimit;
                }
                limit = true;
                if (m_debug & 0x2)
                    printf("+++atcGateway  LongHaulFeedback overinfligh inflightBytes:%lu lastinflightBytes:%lu rateLimit:%lu ratio:%f time:%f\n",inflightBytes,longHaulState.inflightBytes,longHaulState.rateLimit,ratio,Simulator::Now().GetSeconds());
            }
        }
        
        longHaulState.remoteForwardedBytes = remoteForwardedBytes;
        longHaulState.inflightBytes = inflightBytes;
     }
     longHaulState.lastLocalSentBytes = longHaulState.localSentBytes;

    
    if (activeVoqCount <= m_maxVoqCount) {
        // VOQ过载时限流
        longHaulState.activeVoqCount = activeVoqCount;
        bool voqOverload = activeVoqCount > (N_THRESHOLD_RATIO * m_maxVoqCount);
        if (voqOverload) {
            double K = 1.0 / m_maxVoqCount;
            rateLimit = K*longHaulState.longHaulBandwidth;
            if (rateLimit != longHaulState.rateLimit) //波动才限流
            {
                ThrottleLocalForwarding(device, rateLimit); // 限流
            }
            longHaulState.rateLimit = rateLimit;
            limit = true;
            if (m_debug & 0x2)
                printf("+++atcGateway  LongHaulFeedback voqOverload activeVoqCount:%d K:%f rateLimit:%lu time:%f\n",activeVoqCount,K,longHaulState.rateLimit,Simulator::Now().GetSeconds());
        }
    }

    if (!limit && longHaulState.rateLimit < longHaulState.longHaulBandwidth) {
        longHaulState.rateLimit = longHaulState.longHaulBandwidth;
        ThrottleLocalForwarding(device, longHaulState.rateLimit);
        if (m_debug & 0x2)
             printf("+++atcGateway LongHaulFeedback resume sendRate rateLimit:%lu time:%f\n",longHaulState.rateLimit,Simulator::Now().GetSeconds());
    }

    if (longHaulState.firstFeedback) {
        longHaulState.firstFeedback = false;
        longHaulState.lastFeedbackTime = Simulator::Now();
    }
    else
    {
        // 定期重置发送统计
        if (Simulator::Now() - longHaulState.lastFeedbackTime > Seconds(1.0)) {
            longHaulState.localSentBytes = 0;
            longHaulState.lastLocalSentBytes = 0;
            longHaulState.remoteForwardedBytes = 0;
            longHaulState.inflightBytes = 0;
            longHaulState.lastFeedbackTime = Simulator::Now();
        }
    }

    return true; 
}

// 初始化长距离状态
void atcGateway::InitLongHaulState(uint64_t longHaulBandwidth,Time longHaulDelay) {
    longHaulState.longHaulBandwidth = longHaulBandwidth;
    longHaulState.longHaulDelay = longHaulDelay;
    longHaulState.localSentBytes = 0;
    longHaulState.lastLocalSentBytes = 0;
    longHaulState.remoteForwardedBytes = 0;
    longHaulState.inflightBytes = 0;
    longHaulState.maxVoqBytes = 0;
    longHaulState.maxTotalVoqBytes = 0;
    longHaulState.activeVoqCount = 0;
    longHaulState.longHaulChFlag = false;
    longHaulState.longHaulIngressIfIndex = 0;
    longHaulState.firstFeedback = true;
    longHaulState.lastFeedbackTime = Simulator::Now();
    longHaulState.maxInflightBytes = (M * (longHaulState.longHaulBandwidth * longHaulState.longHaulDelay.GetSeconds()) + 
                        longHaulState.longHaulBandwidth * LONGHAUL_FEEDBACK_PERIOD.GetSeconds())/8;
    longHaulState.rateLimit = longHaulState.longHaulBandwidth; // 初始速率400Gbps
    if (m_debug & 0x2)
        printf("+++atcGateway longHaulState maxInflightBytes:%lu , rateLimit:%f Gbps, time:%f\n",longHaulState.maxInflightBytes,longHaulState.rateLimit*1e-9,Simulator::Now().GetSeconds());
}


// 启动长距离反馈定时器
void atcGateway::StartLongHaulFeedbackTimer() {
    longHaulFeedbackEvent = Simulator::Schedule(LONGHAUL_FEEDBACK_PERIOD, 
                                                &atcGateway::GenerateLongHaulFeedback, this);
}

// 生成长距离反馈
void atcGateway::GenerateLongHaulFeedback() {
    if (longHaulState.longHaulChFlag) {
        // 用TCP头封装LongHaulFeedback
        TcpHeader seqh;
        seqh.SetDestinationPort(longHaulState.activeVoqCount);
        seqh.SetSequenceNumber(SequenceNumber32((uint32_t)(longHaulState.remoteForwardedBytes & 0xFFFFFFFF)));
        seqh.SetAckNumber(SequenceNumber32((uint32_t)((longHaulState.remoteForwardedBytes >> 32) & 0xFFFFFFFF)));

        Ptr<Packet> newp = Create<Packet>(std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
        newp->AddHeader(seqh);

        Ipv4Header head;
        head.SetDestination(Ipv4Address(longHaulState.longHaulCh.sip));
        head.SetSource(Ipv4Address(longHaulState.longHaulCh.dip));
        head.SetProtocol(0x6); //tcp
        head.SetTtl(64);
        head.SetPayloadSize(newp->GetSize());
        head.SetIdentification(UniformVariable(0, 65536).GetValue());
        newp->AddHeader(head);

        PppHeader ppp;
        ppp.SetProtocol (EtherToPpp (0x800));
        newp->AddHeader (ppp);

        MyPriorityTag pri;
        pri.SetPriority(0);
        newp->AddPacketTag(pri);
        newp->AddPacketTag(InterfaceTag(longHaulState.longHaulIngressIfIndex));

        CustomHeader bfCh(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
        newp->PeekHeader(bfCh);
        DoSwitchSendToDev(newp, bfCh);

        if (longHaulState.firstFeedback) {
            longHaulState.firstFeedback = false;
            longHaulState.lastFeedbackTime = Simulator::Now();
        }
        else
        {
            // 定期重置发送统计
            if (Simulator::Now() - longHaulState.lastFeedbackTime > Seconds(1.0)) {
                longHaulState.remoteForwardedBytes = 0;
                longHaulState.lastFeedbackTime = Simulator::Now();
            }
        }

        if (m_debug & 0x2)
            printf("+++atcGateway  send LongHaulFeedback remoteForwardedBytes:%lu, activeVoqCount:%d  time:%f\n",longHaulState.remoteForwardedBytes,longHaulState.activeVoqCount,Simulator::Now().GetSeconds());
    }

    // 重置定时器
    StartLongHaulFeedbackTimer();
}


}