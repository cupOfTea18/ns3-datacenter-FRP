/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* Copyright (c) 2006 Georgia Tech Research Corporation, INRIA
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
* Author: Yuliang Li <yuliangli@g.harvard.com>
* Modified (by Vamsi Addanki) to also serve TCP/IP traffic.
*/

#define __STDC_LIMIT_MACROS 1
#include <cmath>
#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include "ns3/qbb-net-device.h"
#include "ns3/log.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/data-rate.h"
#include "ns3/object-vector.h"
#include "ns3/pause-header.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/assert.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/qbb-channel.h"
#include "ns3/random-variable.h"
#include "ns3/qbb-header.h"
#include "ns3/error-model.h"
#include "ns3/cn-header.h"
#include "ns3/ppp-header.h"
#include "ns3/udp-header.h"
#include "ns3/seq-ts-header.h"
#include "ns3/pointer.h"
#include "ns3/custom-header.h"
#include "ns3/rdma-tag.h"
#include "ns3/interface-tag.h"
#include "ns3/unsched-tag.h"
#include "ns3/sim-setting.h"

#include <iostream>

NS_LOG_COMPONENT_DEFINE("QbbNetDevice");

namespace ns3 {

uint32_t RdmaEgressQueue::ack_q_idx = 3;
uint32_t RdmaEgressQueue::tcpip_q_idx = 1;
// RdmaEgressQueue
TypeId RdmaEgressQueue::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::RdmaEgressQueue")
	                    .SetParent<Object> ()
	                    .AddTraceSource ("RdmaEnqueue", "Enqueue a packet in the RdmaEgressQueue.",
	                                     MakeTraceSourceAccessor (&RdmaEgressQueue::m_traceRdmaEnqueue), "ns3::Packet::TracedCallback")
	                    .AddTraceSource ("RdmaDequeue", "Dequeue a packet in the RdmaEgressQueue.",
	                                     MakeTraceSourceAccessor (&RdmaEgressQueue::m_traceRdmaDequeue), "ns3::Packet::TracedCallback")
	                    ;
	return tid;
}

RdmaEgressQueue::RdmaEgressQueue() {
	m_rrlast = 0;
	m_qlast = 0;
	m_ackQ = CreateObject<DropTailQueue<Packet>>();
	m_ackQ->SetAttribute("MaxSize", QueueSizeValue (QueueSize (BYTES, 0xffffffff))); // queue limit is on a higher level, not here
}

Ptr<Packet> RdmaEgressQueue::DequeueQindex(int qIndex) {

	NS_ASSERT_MSG(qIndex != -2, "qIndex -2 appeared in DequeueQindex. This is not intended. Aborting!");
	if (qIndex == -1) { // high prio
		Ptr<Packet> p = m_ackQ->Dequeue();
		m_qlast = -1;
		m_traceRdmaDequeue(p, 0);
		UnSchedTag tag;
		bool found = p->PeekPacketTag(tag);
		uint32_t unsched = tag.GetValue();
		return p;
	}
	if (qIndex >= 0) { // qp
		Ptr<Packet> p = m_rdmaGetNxtPkt(m_qpGrp->Get(qIndex));
		m_rrlast = qIndex;
		m_qlast = qIndex;
		m_traceRdmaDequeue(p, m_qpGrp->Get(qIndex)->m_pg);
		UnSchedTag tag;
		bool found = p->PeekPacketTag(tag);
		uint32_t unsched = tag.GetValue();
		return p;
	}
	return 0;
}
int RdmaEgressQueue::GetNextQindex(bool paused[]) {
	bool found = false;
	uint32_t qIndex;
	if (!paused[ack_q_idx] && m_ackQ->GetNPackets() > 0)
		return -1;

	// no pkt in highest priority queue, do rr for each qp
	int res = -1024;

	for (uint32_t dorr = 0; dorr < 2; dorr++) {
		hostDequeueIndex++;
		if (hostDequeueIndex % 2) {
			uint32_t fcount = m_qpGrp->GetN();
			uint32_t min_finish_id = 0xffffffff;
			for (qIndex = 1; qIndex <= fcount; qIndex++) {
				uint32_t idx = (qIndex + m_rrlast) % fcount;
				Ptr<RdmaQueuePair> qp = m_qpGrp->Get(idx);
				if (!paused[qp->m_pg] && qp->GetBytesLeft() > 0 && !qp->IsWinBound()) {
					if (m_qpGrp->Get(idx)->m_nextAvail.GetTimeStep() > Simulator::Now().GetTimeStep()) //not available now
						continue;
					res = idx;
					break;
				} else if (qp->IsFinished()) {
					min_finish_id = idx < min_finish_id ? idx : min_finish_id;
				}
			}

			// clear the finished qp
			if (min_finish_id < 0xffffffff) {
				int nxt = min_finish_id;
				auto &qps = m_qpGrp->m_qps;
				for (int i = min_finish_id + 1; i < fcount; i++) if (!qps[i]->IsFinished()) {
						if (i == res) // update res to the idx after removing finished qp
							res = nxt;
						qps[nxt] = qps[i];
						nxt++;
					}
				qps.resize(nxt);
			}

			if (res != -1024) {
				return res;
			}
		}
		else {
			if (qb_dev->GetQueue()->GetNBytes(tcpip_q_idx)) {
				res = -2;
				return res;
			}
		}
	}

	return res;
}

int RdmaEgressQueue::GetLastQueue() {
	return m_qlast;
}

uint32_t RdmaEgressQueue::GetNBytes(uint32_t qIndex) {
	NS_ASSERT_MSG(qIndex < m_qpGrp->GetN(), "RdmaEgressQueue::GetNBytes: qIndex >= m_qpGrp->GetN()");
	return m_qpGrp->Get(qIndex)->GetBytesLeft();
}

uint32_t RdmaEgressQueue::GetFlowCount(void) {
	return m_qpGrp->GetN();
}

Ptr<RdmaQueuePair> RdmaEgressQueue::GetQp(uint32_t i) {
	return m_qpGrp->Get(i);
}

void RdmaEgressQueue::RecoverQueue(uint32_t i) {
	NS_ASSERT_MSG(i < m_qpGrp->GetN(), "RdmaEgressQueue::RecoverQueue: qIndex >= m_qpGrp->GetN()");
	m_qpGrp->Get(i)->snd_nxt = m_qpGrp->Get(i)->snd_una;
}

void RdmaEgressQueue::EnqueueHighPrioQ(Ptr<Packet> p) {
	m_traceRdmaEnqueue(p, 0);
	m_ackQ->Enqueue(p);
}

void RdmaEgressQueue::CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb) {
	while (m_ackQ->GetNPackets() > 0) {
		Ptr<Packet> p = m_ackQ->Dequeue();
		dropCb(p, 0);
	}
}

/******************
 * QbbNetDevice
 *****************/
NS_OBJECT_ENSURE_REGISTERED(QbbNetDevice);

TypeId
QbbNetDevice::GetTypeId(void)
{
	static TypeId tid = TypeId("ns3::QbbNetDevice")
	                    .SetParent<PointToPointNetDevice>()
	                    .AddConstructor<QbbNetDevice>()
	                    .AddAttribute("QbbEnabled",
	                                  "Enable the generation of PAUSE packet.",
	                                  BooleanValue(true),
	                                  MakeBooleanAccessor(&QbbNetDevice::m_qbbEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("QcnEnabled",
	                                  "Enable the generation of PAUSE packet.",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&QbbNetDevice::m_qcnEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("PauseTime",
	                                  "Number of microseconds to pause upon congestion",
	                                  UintegerValue(5),
	                                  MakeUintegerAccessor(&QbbNetDevice::m_pausetime),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("NicDelay",
	                                  "The processing delay of the NIC (from wire to protocol stack)",
	                                  TimeValue(NanoSeconds(0)),
	                                  MakeTimeAccessor(&QbbNetDevice::m_nicDelay),
	                                  MakeTimeChecker())
	                    .AddAttribute ("TxBeQueue",
	                                   "A queue to use as the transmit queue in the device.",
	                                   PointerValue (),
	                                   MakePointerAccessor (&QbbNetDevice::m_queue),
	                                   MakePointerChecker<Queue<Packet>>())
	                    .AddAttribute ("RdmaEgressQueue",
	                                   "A queue to use as the transmit queue in the device.",
	                                   PointerValue (),
	                                   MakePointerAccessor (&QbbNetDevice::m_rdmaEQ),
	                                   MakePointerChecker<Object> ())
	                    .AddTraceSource ("QbbAdmitted", "Successful logical queue admission (packet, logical queue, PG).",
                                         MakeTraceSourceAccessor (&QbbNetDevice::m_traceAdmitted), "ns3::Packet::TracedCallback")
	                    .AddTraceSource ("QbbEnqueue", "Enqueue a packet in the QbbNetDevice.",
	                                     MakeTraceSourceAccessor (&QbbNetDevice::m_traceEnqueue), "ns3::Packet::TracedCallback")
	                    .AddTraceSource ("QbbDequeue", "Dequeue a packet in the QbbNetDevice.",
	                                     MakeTraceSourceAccessor (&QbbNetDevice::m_traceDequeue), "ns3::Packet::TracedCallback")
	                    .AddTraceSource ("QbbDrop", "Drop a packet in the QbbNetDevice.",
	                                     MakeTraceSourceAccessor (&QbbNetDevice::m_traceDrop), "ns3::Packet::TracedCallback")
	                    .AddTraceSource ("RdmaQpDequeue", "A qp dequeue a packet.",
	                                     MakeTraceSourceAccessor (&QbbNetDevice::m_traceQpDequeue), "ns3::Packet::TracedCallback")
	                    .AddTraceSource ("QbbPfc", "get a PFC packet. 0: resume, 1: pause",
	                                     MakeTraceSourceAccessor (&QbbNetDevice::m_tracePfc), "ns3::Packet::TracedCallback")
	                    ;

	return tid;
}

QbbNetDevice::QbbNetDevice()
{
	NS_LOG_FUNCTION(this);
	m_ecn_source = new std::vector<ECNAccount>;
	m_rdmaEQ = CreateObject<RdmaEgressQueue>();
	m_rdmaEQ->qb_dev = this;

	for (uint32_t i = 0; i < qCnt; i++) {
		m_paused[i] = false;
		dummy_paused[i] = false;
		m_rdmaEQ->dummy_paused[i] = dummy_paused[i];
	}
	hostDequeueIndex = 0;
}

QbbNetDevice::~QbbNetDevice()
{
	NS_LOG_FUNCTION(this);
}

void
QbbNetDevice::DoDispose()
{
    Simulator::Cancel(m_shapeWake);
    m_groupClassifier = Callback<uint32_t, Ptr<const Packet>, uint32_t>();
	NS_LOG_FUNCTION(this);

	for (uint32_t i = 0; i < qCnt; ++i) Simulator::Cancel(m_pauseExpiry[i]);
	PointToPointNetDevice::DoDispose();
}

DataRate QbbNetDevice::GetDataRate() {
	return m_bps;
}

bool
QbbNetDevice::TransmitStart(Ptr<Packet> p)
{
	NS_LOG_FUNCTION(this << p);
	NS_LOG_LOGIC("UID is " << p->GetUid() << ")");
	//
	// This function is called to start the process of transmitting a packet.
	// We need to tell the channel that we've started wiggling the wire and
	// schedule an event that will be executed when the transmission is complete.
	//
	NS_ASSERT_MSG(m_txMachineState == READY, "Must be READY to transmit");
	m_txMachineState = BUSY;
	m_currentPkt = p;
	m_phyTxBeginTrace(m_currentPkt);

	Time txTime = m_bps.CalculateBytesTxTime(p->GetSize());
	Time txCompleteTime = txTime + m_tInterframeGap;

	NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.GetSeconds() << "sec");
	Simulator::Schedule(txCompleteTime, &QbbNetDevice::TransmitComplete, this);

	bool result = m_channel->TransmitStart(p, this, txTime);
	if (result == false)
	{
		m_phyTxDropTrace(p);
	}
	return result;
}

void
QbbNetDevice::TransmitComplete(void)
{
	NS_LOG_FUNCTION(this);
	NS_ASSERT_MSG(m_txMachineState == BUSY, "Must be BUSY if transmitting");
	m_txMachineState = READY;
	NS_ASSERT_MSG(m_currentPkt != 0, "QbbNetDevice::TransmitComplete(): m_currentPkt zero");
	m_phyTxEndTrace(m_currentPkt);
	m_currentPkt = 0;
	DequeueAndTransmit();
}

void QbbNetDevice::UpdateShapeTokens(QueueShaper& shaper) {
    const Time now=Simulator::Now();
    shaper.tokens=std::min(double(shaper.burst),shaper.tokens+shaper.rate*(now-shaper.updated).GetSeconds());
    shaper.updated=now;
}
void QbbNetDevice::ConfigureGroupShaper(uint32_t queue,uint32_t priority,double rate,uint32_t burst) {
    NS_ABORT_MSG_IF(priority==0 || priority>=qCnt || burst==0 || m_shapers.count(queue),"invalid/repeated shaper");
    m_queue->ConfigureLogicalQueue(queue,priority);
    auto &s=m_shapers[queue]; s.priority=priority; s.burst=burst; s.tokens=burst; s.updated=Simulator::Now();
    SetGroupShaperRate(queue,rate);
}
void QbbNetDevice::SetGroupShaperRate(uint32_t queue,double rate) {
    NS_ABORT_MSG_IF(!m_shapers.count(queue) || !std::isfinite(rate) || rate<0,"invalid shaper rate");
    auto &s=m_shapers.at(queue); UpdateShapeTokens(s); s.rate=rate;
    if (rate==0) s.tokens=0;
    Simulator::Cancel(m_shapeWake); DequeueAndTransmit();
}

void QbbNetDevice::UpdateProbeTokens() {
    auto now=Simulator::Now();
    m_probeTokens=std::min(double(m_probeBurst),m_probeTokens+m_probeRate*(now-m_probeUpdated).GetSeconds());
    m_probeUpdated=now;
}
double QbbNetDevice::GetProbeTokens() { UpdateProbeTokens(); return m_probeTokens; }
void QbbNetDevice::ConfigureProbeBucket(double rate,uint64_t burst) {
    NS_ABORT_MSG_IF(!std::isfinite(rate) || rate<=0 || !burst || m_probeBurst,"invalid/repeated probe bucket");
    for (const auto& e:m_shapers)
        NS_ABORT_MSG_IF(burst<e.second.burst,"probe bucket cannot hold maximum frame");
    m_probeRate=rate; m_probeBurst=burst; m_probeTokens=burst; m_probeUpdated=Simulator::Now();
}
QbbNetDevice::ProbeCounters QbbNetDevice::GetProbeCounters(uint32_t queue) const {
    return m_shapers.at(queue).grant;
}
void QbbNetDevice::GrantProbeBytes(uint32_t queue,uint64_t epoch,uint64_t bytes) {
    NS_ABORT_MSG_IF(!m_shapers.count(queue) || !epoch || !bytes || !m_probeBurst,"invalid probe grant");
    auto& g=m_shapers.at(queue).grant;
    if (epoch<=g.epoch) return; // Delayed/duplicate grants cannot refresh the allowance.
    g.epoch=epoch; g.remaining=bytes;
    Simulator::Cancel(m_shapeWake); DequeueAndTransmit();
}
void QbbNetDevice::ApplyQueueControls(const std::vector<QueueControl>& controls) {
    NS_ABORT_MSG_IF(controls.size()!=m_shapers.size(),"batch must include every registered queue");
    std::set<uint32_t> seen;
    double sum=0;
    for (const auto& c:controls) {
        NS_ABORT_MSG_IF(!m_shapers.count(c.queue) || !seen.insert(c.queue).second ||
            !std::isfinite(c.bytesPerSec) || c.bytesPerSec<0 || (c.probe && !m_probeBurst),
            "invalid queue control batch");
        sum+=c.bytesPerSec;
    }
    NS_ABORT_MSG_IF(!std::isfinite(sum) || sum>GetDataRate().GetBitRate()/8.0+1e-6,"batch exceeds physical capacity");
    UpdateProbeTokens();
    for (const auto& c:controls) {
        auto& s=m_shapers.at(c.queue); UpdateShapeTokens(s);
        s.rate=c.bytesPerSec; s.probe=c.probe;
        if (!s.rate) s.tokens=0;
    }
    Simulator::Cancel(m_shapeWake); DequeueAndTransmit();
}

void
QbbNetDevice::DequeueAndTransmit(void)
{
	// std::cout << "dequeue " << std::endl;
	NS_LOG_FUNCTION(this);
	if (!m_linkUp) return; // if link is down, return
	if (m_txMachineState == BUSY) return;	// Quit if channel busy
	Ptr<Packet> p;
	if (m_node->GetNodeType() == 0) {
		int qIndex = m_rdmaEQ->GetNextQindex(m_paused);
		// std::cout << "qIndex " << qIndex << std::endl;
		if (qIndex != -1024) {
			if (qIndex == -1) { // high prio
				p = m_rdmaEQ->DequeueQindex(qIndex);
				m_traceDequeue(p, 0);
				TransmitStart(p);
				numTxBytes += p->GetSize();
				totalBytesSent += p->GetSize();
				return;
			}
			else if (qIndex == -2) {
				Ptr<Packet> p = m_queue->DequeueRR (m_paused);
				if (p == 0)
				{
					NS_LOG_LOGIC ("No pending packets in device queue after tx complete");
					return;
				}

				// //
				// // Got another packet off of the queue, so start the transmit process again.
				// //
				m_snifferTrace (p);
				m_promiscSnifferTrace (p);
				TransmitStart (p);
				totalBytesSent += p->GetSize();
				return;
			}
			// a qp dequeue a packet
			Ptr<RdmaQueuePair> lastQp = m_rdmaEQ->GetQp(qIndex);
			p = m_rdmaEQ->DequeueQindex(qIndex);
			// if (p==NULL)
			// std::cout << "p is null" << std::endl;

			// transmit
			m_traceQpDequeue(p, lastQp);
			TransmitStart(p);

			// update for the next avail time
			m_rdmaPktSent(lastQp, p, m_tInterframeGap);
			totalBytesSent += p->GetSize();
		} else { // no packet to send
			NS_LOG_INFO("PAUSE prohibits send at node " << m_node->GetId());
			Time t = Simulator::GetMaximumSimulationTime();
			for (uint32_t i = 0; i < m_rdmaEQ->GetFlowCount(); i++) {
				Ptr<RdmaQueuePair> qp = m_rdmaEQ->GetQp(i);
				if (qp->GetBytesLeft() == 0)
					continue;
				t = Min(qp->m_nextAvail, t);
			}
			if (m_nextSend.IsExpired() && t < Simulator::GetMaximumSimulationTime() && t > Simulator::Now()) {
				m_nextSend = Simulator::Schedule(t - Simulator::Now(), &QbbNetDevice::DequeueAndTransmit, this);
			}
		}
		return;
	}
	else {  //switch, doesn't care about qcn, just send
        std::set<uint32_t> blocked;
        Time wait=Simulator::GetMaximumSimulationTime();
        UpdateProbeTokens();
        for (auto &entry:m_shapers) {
            auto &s=entry.second; UpdateShapeTokens(s);
            auto head=m_queue->PeekQueue(entry.first);
            if (!head) continue;
            const auto bytes=head->GetSize();
            NS_ABORT_MSG_IF(bytes>s.burst,"frame exceeds shaper burst");
            const bool hardBlocked=m_paused[s.priority] || !s.rate ||
                (s.probe && (s.grant.remaining<bytes || !m_probeRate));
            double seconds= s.rate>0 ? std::max(0.0,(bytes-s.tokens)/s.rate) : 0;
            if (s.probe && m_probeRate>0)
                seconds=std::max(seconds,std::max(0.0,(bytes-m_probeTokens)/m_probeRate));
            if (hardBlocked || seconds>1e-15) {
                blocked.insert(entry.first);
                if (!hardBlocked)
                    wait=std::min(wait,std::max(NanoSeconds(1),NanoSeconds(int64_t(std::ceil(seconds*1e9)))));
            }
        }
        if (wait<Simulator::GetMaximumSimulationTime() && m_shapeWake.IsExpired())
            m_shapeWake=Simulator::Schedule(wait,&QbbNetDevice::DequeueAndTransmit,this);
        p=m_queue->DequeueRR(m_paused,blocked);
        if (p && m_shapers.count(m_queue->GetLastLogicalQueue())) {
            auto &s=m_shapers.at(m_queue->GetLastLogicalQueue());
            s.tokens=std::max(0.0,s.tokens-p->GetSize());
            if (s.probe) {
                NS_ABORT_MSG_IF(s.grant.remaining<p->GetSize() || m_probeTokens+1e-6<p->GetSize(),
                                "probe dequeue exceeded grant/bucket");
                s.grant.remaining-=p->GetSize();
                NS_ABORT_MSG_IF(UINT64_MAX-s.grant.sent<p->GetSize() || UINT64_MAX-m_probeSent<p->GetSize(),
                                "probe counter overflow");
                s.grant.sent+=p->GetSize(); m_probeSent+=p->GetSize();
                m_probeTokens=std::max(0.0,m_probeTokens-p->GetSize());
            }
        }
		if (p != 0) {
			m_snifferTrace(p);
			m_promiscSnifferTrace(p);
			Ipv4Header h;
			Ptr<Packet> packet = p->Copy();
			uint16_t protocol = 0;
			ProcessHeader(packet, protocol);
			packet->RemoveHeader(h);
			InterfaceTag t;
			uint32_t qIndex = m_queue->GetLastQueue();
			if (qIndex == 0) { //this is a pause or cnp, send it immediately!
				m_node->SwitchNotifyDequeue(m_ifIndex, qIndex, p);
				p->RemovePacketTag(t);
			} else {
				m_node->SwitchNotifyDequeue(m_ifIndex, qIndex, p);
				p->RemovePacketTag(t);
			}
			m_traceDequeue(p, qIndex);
			TransmitStart(p);
			numTxBytes += p->GetSize();
			totalBytesSent += p->GetSize();
			return;
		} else { //No queue can deliver any packet
			NS_LOG_INFO("PAUSE prohibits send at node " << m_node->GetId());
			if (m_node->GetNodeType() == 0 && m_qcnEnabled) { //nothing to send, possibly due to qcn flow control, if so reschedule sending
				Time t = Simulator::GetMaximumSimulationTime();
				for (uint32_t i = 0; i < m_rdmaEQ->GetFlowCount(); i++) {
					Ptr<RdmaQueuePair> qp = m_rdmaEQ->GetQp(i);
					if (qp->GetBytesLeft() == 0)
						continue;
					t = Min(qp->m_nextAvail, t);
				}
				if (m_nextSend.IsExpired() && t < Simulator::GetMaximumSimulationTime() && t > Simulator::Now()) {
					m_nextSend = Simulator::Schedule(t - Simulator::Now(), &QbbNetDevice::DequeueAndTransmit, this);
				}
			}
		}
	}
	return;
}

void
QbbNetDevice::Resume(unsigned qIndex)
{
	NS_LOG_FUNCTION(this << qIndex);
	if (!m_paused[qIndex]) return;
	Simulator::Cancel(m_pauseExpiry[qIndex]);
	m_pauseTotal[qIndex] += Simulator::Now() - m_pauseStarted[qIndex];
	m_paused[qIndex] = false;
	NS_LOG_INFO("Node " << m_node->GetId() << " dev " << m_ifIndex << " queue " << qIndex <<
	            " resumed at " << Simulator::Now().GetSeconds());
	DequeueAndTransmit();
}

void
QbbNetDevice::SetReceiveCallback (NetDevice::ReceiveCallback cb)
{
	m_rxCallback = cb;
}

bool
QbbNetDevice::ProcessHeader (Ptr<Packet> p, uint16_t& param)
{
	NS_LOG_FUNCTION (this << p << param);
	PppHeader ppp;
	p->RemoveHeader (ppp);
	// std::cout << "p2p prot " << uint32_t(ppp.GetProtocol ()) << std::endl;
	param = PppToEther (ppp.GetProtocol ());
	return true;
}

uint16_t
QbbNetDevice::PppToEther (uint16_t proto)
{
	NS_LOG_FUNCTION_NOARGS();
	switch (proto)
	{
	case 0x0021: return 0x0800;   //IPv4
	case 0x0057: return 0x86DD;   //IPv6
	default:
		NS_ASSERT_MSG (false, "PPP Protocol number not defined!");
		std::cout << "PPP Protocol number not defined!" << std::endl;
	}
	return 0;
}

uint16_t
QbbNetDevice::EtherToPpp (uint16_t proto)
{
	NS_LOG_FUNCTION_NOARGS();
	switch (proto)
	{
	case 0x0800: return 0x0021;   //IPv4
	case 0x86DD: return 0x0057;   //IPv6
	default: NS_ASSERT_MSG (false, "PPP Protocol number not defined!");
	}
	return 0;
}

void
QbbNetDevice::Receive(Ptr<Packet> packet)
{
// std::cout << "receive" << std::endl;
	NS_LOG_FUNCTION(this << packet);
	
	// 1. 检查是否有网卡处理延迟
	if (!m_nicDelay.IsZero() && !m_nicDelay.IsNegative()) {
		// 有延迟，调度延迟执行（不阻塞当前流程）
		Simulator::Schedule(m_nicDelay, &QbbNetDevice::DoReceive, this, packet);
		return;
	}
	
	// 2. 无延迟，直接处理
	DoReceive(packet);
}

/**
 * 实际接收处理函数（延迟后调用）
 * 包含原本Receive()的全部逻辑
 */
void
QbbNetDevice::DoReceive(Ptr<Packet> packet)
{
	NS_LOG_FUNCTION(this << packet);
	
	if (!m_linkUp) {
		m_traceDrop(packet, 0);
		return;
	}

	if (m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet))
	{
		//
		// If we have an error model and it indicates that it is time to lose a
		// corrupted packet, don't forward this packet up, let it go.
		//
		m_phyRxDropTrace(packet);
		return;
	}

	m_macRxTrace(packet);

	CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
	ch.getInt = 1; // parse INT header
	packet->PeekHeader(ch);
	
	static uint32_t dbgPktCount = 0;
	if (!g_longhaul_quiet && (dbgPktCount < 3 || ch.l3Prot == 0x01)) {
		std::cout << "[DoReceive] node=" << m_node->GetId() << " type=" << (int)m_node->GetNodeType() << " l3Prot=0x" << std::hex << (int)ch.l3Prot << std::dec << " pktSize=" << packet->GetSize() << std::endl;
		dbgPktCount++;
	}
	if (ch.l3Prot == 0xFE) { // PFC
		if (!m_qbbEnabled) return;
		unsigned qIndex = ch.pfc.qIndex;
		if (qIndex >= qCnt) return;
		Simulator::Cancel(m_pauseExpiry[qIndex]);
		if (ch.pfc.time > 0) {
			m_tracePfc(1);
			if (!m_paused[qIndex]) m_pauseStarted[qIndex] = Simulator::Now();
			m_paused[qIndex] = true;
			m_pauseExpiry[qIndex] = Simulator::Schedule(MicroSeconds(ch.pfc.time), &QbbNetDevice::Resume, this, qIndex);
		} else {
			m_tracePfc(0);
			Resume(qIndex);
		}
	} else { // non-PFC packets (data, ACK, NACK, CNP...)
		if (m_node->GetNodeType() > 0) { // switch
			packet->AddPacketTag(InterfaceTag(m_ifIndex));
			m_node->SwitchReceiveFromDevice(this, packet, ch);
		} else { // NIC
			int ret;
			Ptr<Packet> cp = packet->Copy();
			PppHeader ph; cp->RemoveHeader(ph);
			Ipv4Header ih;
			cp->RemoveHeader(ih);
			if (ih.GetProtocol() == 0x06) {
				m_snifferTrace (packet);
				m_promiscSnifferTrace (packet);
				m_phyRxEndTrace (packet);
				Ptr<Packet> originalPacket = packet->Copy ();
				uint16_t prot = 0;
				ProcessHeader (packet, prot);

				if (!m_promiscCallback.IsNull ())
				{
					m_macPromiscRxTrace (originalPacket);
					m_promiscCallback (this, packet, prot, GetRemote (), GetAddress (), NetDevice::PACKET_HOST);
				}
				m_macRxTrace (originalPacket);
				m_rxCallback (this, packet, prot, GetRemote ());
			}
			else {
				// send to RdmaHw
				ret = m_rdmaReceiveCb(packet, ch);
			}
			// TODO we may based on the ret do something
		}
	}
	return;
}

Address
QbbNetDevice::GetRemote (void) const
{
	NS_LOG_FUNCTION (this);
	NS_ASSERT (m_channel->GetNDevices () == 2);
	for (std::size_t i = 0; i < m_channel->GetNDevices (); ++i)
	{
		Ptr<NetDevice> tmp = m_channel->GetDevice (i);
		if (tmp != this)
		{
			return tmp->GetAddress ();
		}
	}
	NS_ASSERT (false);
	// quiet compiler.
	return Address ();
}

bool QbbNetDevice::Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber)
{
	NS_LOG_FUNCTION (this << packet << dest << protocolNumber);
	NS_LOG_LOGIC ("p=" << packet << ", dest=" << &dest);
	NS_LOG_LOGIC ("UID is " << packet->GetUid ());

	if (IsLinkUp () == false)
	{
		m_macTxDropTrace (packet);
		return false;
	}
	AddHeader (packet, protocolNumber);
	m_macTxTrace (packet);
	m_queue->Enqueue (packet, m_rdmaEQ->tcpip_q_idx);
	DequeueAndTransmit();
	return true;
}

bool QbbNetDevice::SwitchSend (uint32_t qIndex, Ptr<Packet> packet, CustomHeader &ch) {
	m_macTxTrace(packet);
	m_traceEnqueue(packet, qIndex);
	uint32_t logical=m_groupClassifier.IsNull() ? qIndex : m_groupClassifier(packet,qIndex);
    NS_ABORT_MSG_IF(!m_queue->Enqueue(packet,logical),"queue admission failed after MMU charge");
    m_traceAdmitted(packet,logical,qIndex);
	DequeueAndTransmit();
	return true;
}

void QbbNetDevice::SendPfc(uint32_t qIndex, uint32_t type) {
	Ptr<Packet> p = Create<Packet>(0);
	PauseHeader pauseh((type == 0 ? m_pausetime : 0), m_queue->GetNBytes(qIndex), qIndex);
	p->AddHeader(pauseh);
	Ipv4Header ipv4h;  // Prepare IPv4 header
	ipv4h.SetProtocol(0xFE);
	ipv4h.SetSource(m_node->GetObject<Ipv4>()->GetAddress(m_ifIndex, 0).GetLocal());
	ipv4h.SetDestination(Ipv4Address("255.255.255.255"));
	ipv4h.SetPayloadSize(p->GetSize());
	ipv4h.SetTtl(1);
	ipv4h.SetIdentification(UniformVariable(0, 65536).GetValue());
	p->AddHeader(ipv4h);
	AddHeader(p, 0x800);
	CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
	p->PeekHeader(ch);
	m_tracePfc(type+2); // 2 indicates PFC PAUSE sent.3 indicates RESUME sent
	SwitchSend(0, p, ch);
}

// ========== Bifrost: 自定义暂停时间的PFC发送 ==========
void QbbNetDevice::SendPfcWithTime(uint32_t qIndex, uint32_t type, uint32_t customTime) {
	Ptr<Packet> p = Create<Packet>(0);
	// 关键区别:使用传入的customTime而非m_pausetime
	PauseHeader pauseh((type == 0 ? customTime : 0), m_queue->GetNBytes(qIndex), qIndex);
	p->AddHeader(pauseh);
	Ipv4Header ipv4h;
	ipv4h.SetProtocol(0xFE);
	ipv4h.SetSource(m_node->GetObject<Ipv4>()->GetAddress(m_ifIndex, 0).GetLocal());
	ipv4h.SetDestination(Ipv4Address("255.255.255.255"));
	ipv4h.SetPayloadSize(p->GetSize());
	ipv4h.SetTtl(1);
	ipv4h.SetIdentification(UniformVariable(0, 65536).GetValue());
	p->AddHeader(ipv4h);
	AddHeader(p, 0x800);
	CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
	p->PeekHeader(ch);
	m_tracePfc(type+2); // 2 indicates PFC PAUSE sent.3 indicates RESUME sent
	SwitchSend(0, p, ch);
}

bool
QbbNetDevice::Attach(Ptr<QbbChannel> ch)
{
	NS_LOG_FUNCTION(this << &ch);
	m_channel = ch;
	m_channel->Attach(this);
	NotifyLinkUp();
	return true;
}

Ptr<Channel>
QbbNetDevice::GetChannel(void) const
{
	return m_channel;
}

bool QbbNetDevice::IsQbb(void) const {
	return true;
}

void QbbNetDevice::NewQp(Ptr<RdmaQueuePair> qp) {
	qp->m_nextAvail = Simulator::Now();
	DequeueAndTransmit();
}
void QbbNetDevice::ReassignedQp(Ptr<RdmaQueuePair> qp) {
	DequeueAndTransmit();
}
void QbbNetDevice::TriggerTransmit(void) {
	DequeueAndTransmit();
}

void QbbNetDevice::SetQueue(Ptr<BEgressQueue> q) {
	NS_LOG_FUNCTION(this << q);
	m_queue = q;
}

Ptr<BEgressQueue> QbbNetDevice::GetQueue() {
	return m_queue;
}

Ptr<RdmaEgressQueue> QbbNetDevice::GetRdmaQueue() {
	return m_rdmaEQ;
}

void QbbNetDevice::RdmaEnqueueHighPrioQ(Ptr<Packet> p) {
	m_traceEnqueue(p, 0);
	m_rdmaEQ->EnqueueHighPrioQ(p);
}

void QbbNetDevice::TakeDown() {
	// TODO: delete packets in the queue, set link down
	if (m_node->GetNodeType() == 0) {
		// clean the high prio queue
		m_rdmaEQ->CleanHighPrio(m_traceDrop);
		// notify driver/RdmaHw that this link is down
		m_rdmaLinkDownCb(this);
	} else { // switch
		// clean the queue
		for (uint32_t i = 0; i < qCnt; i++)
			m_paused[i] = false;
		while (1) {
			Ptr<Packet> p = m_queue->DequeueRR(m_paused);
			if (p == 0)
				break;
			m_traceDrop(p, m_queue->GetLastQueue());
		}
		// TODO: Notify switch that this link is down
	}
	m_linkUp = false;
}

void QbbNetDevice::UpdateNextAvail(Time t) {
	if (!m_nextSend.IsExpired() && t < Time(m_nextSend.GetTs())) {
		Simulator::Cancel(m_nextSend);
		Time delta = t < Simulator::Now() ? Time(0) : t - Simulator::Now();
		m_nextSend = Simulator::Schedule(delta, &QbbNetDevice::DequeueAndTransmit, this);
	}
}
} // namespace ns3
