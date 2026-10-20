#include "dci-gateway-node.h"
#include "ppp-header.h"
#include "qbb-header.h"
#include "ns3/interface-tag.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(DciGatewayNode);
NS_OBJECT_ENSURE_REGISTERED(R5Header);
namespace {
using H=R5Header;
int64_t Ns(double s) { return Seconds(s).GetNanoSeconds(); }
int64_t NowNs() { return Simulator::Now().GetNanoSeconds(); }
uint32_t ControlFrameBytes() { return H().GetSerializedSize()+Ipv4Header().GetSerializedSize()+PppHeader().GetSerializedSize(); }
void Add(uint64_t& value,uint64_t bytes) {
    NS_ABORT_MSG_IF(UINT64_MAX-value<bytes,"R5 counter overflow"); value+=bytes;
}
CustomHeader Parse(Ptr<const Packet> p) {
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
    p->PeekHeader(h); return h;
}
}
TypeId R5Header::GetTypeId() {
    static TypeId id=TypeId("ns3::R5Header").SetParent<Header>().AddConstructor<R5Header>(); return id;
}
TypeId R5Header::GetInstanceTypeId() const { return GetTypeId(); }
void R5Header::Serialize(Buffer::Iterator i) const {
    i.WriteHtonU32(0x52350009); i.WriteHtonU16(type); i.WriteHtonU16(FIELDS);
    for (auto value:v) i.WriteHtonU64(value);
}
uint32_t R5Header::Deserialize(Buffer::Iterator i) {
    if (i.GetRemainingSize()!=GetSerializedSize() || i.ReadNtohU32()!=0x52350009) return 0;
    type=i.ReadNtohU16(); if (type<PROBE || type>FAST_CNP || i.ReadNtohU16()!=FIELDS) return 0;
    for (auto& value:v) value=i.ReadNtohU64();
    return GetSerializedSize();
}
void R5Header::Print(std::ostream& out) const { out<<"R5/v9 type="<<type; }
TypeId DciGatewayNode::GetTypeId() {
    static TypeId id=TypeId("ns3::DciGatewayNode").SetParent<SwitchNode>().AddConstructor<DciGatewayNode>(); return id;
}
Ipv4Address DciGatewayNode::ControlAddress(uint32_t node) {
    NS_ABORT_MSG_IF(node>=65534,"R5 control address pool exhausted"); return Ipv4Address(0xac1f0000u+node+1);
}
Ptr<QbbNetDevice> DciGatewayNode::Port(uint32_t port) const {
    NS_ABORT_MSG_IF(port>=GetNDevices(),"R5 invalid port");
    auto d=DynamicCast<QbbNetDevice>(GetDevice(port)); NS_ABORT_MSG_IF(!d,"R5 requires Qbb"); return d;
}
void DciGatewayNode::Configure(const Config& c,const std::string& output) {
    NS_ABORT_MSG_IF(m_enabled || !std::isfinite(c.period+c.control+c.timeout+
        c.conservativeFraction+c.qref+c.margin+c.sourceHigh+c.sourceLow+c.localWait+c.cnpInterval+
        c.localWindow+c.initialDelay+c.probePeriod+c.queryRetry+c.controlFraction) || Ns(c.control)<=0 || Ns(c.period)<=0 ||
        c.timeout<=c.period || c.initialDelay<=0 || c.probePeriod<=0 || c.queryRetry<=0 ||
        c.controlFraction<=0 || c.controlFraction>1 || c.controlBurstBytes<ControlFrameBytes() ||
        c.controlQueueBytes<ControlFrameBytes() || c.localWindow<=0 || c.localWait<=0 || c.cnpInterval<=0 ||
        c.conservativeFraction<=0 || c.conservativeFraction>1 || c.qref<0 || c.margin<=0 ||
        c.sourceLow<0 || c.sourceHigh<=c.sourceLow || c.bufferBytes<=c.margin+c.qref ||
        !c.maxFlows || !c.maxForecasts || !c.maxFuturePackets ||
        !c.maxReports || !c.maxRetries || !c.session || !c.peerSession,"invalid R5 configuration");
    m_config=c; m_controlTokens=c.controlBurstBytes; m_enabled=true; m_forwardBase=m_reverseBase=Ns(c.initialDelay);
    auto prefix=output+".gateway-"+std::to_string(GetId());
    m_log.open(prefix+".flows.csv"); m_events.open(prefix+".events.csv");
    m_packets.open(prefix+".packets.csv"); m_resourceLog.open(prefix+".resources.csv");
    m_predictions.open(prefix+".predictions.csv");
    m_budgetLog.open(prefix+".resource-budgets.csv");
    m_retiredPath=prefix+".retired.jsonl"; m_retired.open(m_retiredPath);
    NS_ABORT_MSG_IF(!m_retired,"cannot open R5 retired-flow archive");
    NS_ABORT_MSG_IF(!m_log || !m_events || !m_packets || !m_resourceLog || !m_predictions || !m_budgetLog,"cannot open R5 logs");
    m_events<<"time_ns,node,event_order,event,id,value,reason\n";
    m_packets<<"time_ns,node,event_order,event,role,flow,generation,src_ip,dst_ip,dest_qpn,bytes,in_total,dequeued_total,tx_total,queue_bytes,arrival_ns\n";
    m_log<<"time_ns,node,decision,flow,generation,peer_generation,resource,port_resource,queue_bytes,candidate_Bps,target_Bps,tokens,burst_bytes,supply_Bps,resource_net_service_Bps,state_sample_ns,state_queue_bytes,df_ns,db_ns,reason\n";
    m_resourceLog<<"time_ns,node,resource,port,pg,queue_bytes,in_total,out_total,other_total,dropped_total,service_Bps,capacity_Bps,background_Bps,pause_ns,interval_ns,flags\n";
    m_predictions<<"time_ns,node,decision,resource,event,sample_ns,end_ns,initial_queue_bytes,service_Bps,predicted_queue_bytes,peak_bytes,planned_bytes,actual_queue_bytes,replayed_queue_bytes,reason\n";
    m_budgetLog<<"time_ns,node,decision,resource,sample_ns,initial_queue_bytes,net_service_Bps,effective_queue_bytes,history_peak_bytes,budget_Bps,allocated_Bps,history_covered,reason\n";
}
void DciGatewayNode::SetPeer(uint32_t peer,uint32_t wan,uint32_t frame) {
    NS_ABORT_MSG_IF(m_started || peer==GetId() || !frame || frame>UINT32_MAX/4,"invalid R5 peer/frame");
    m_peer=peer; m_wan=wan; m_frame=frame; Port(wan);
}
void DciGatewayNode::SetDestinationBudget(uint32_t ip,double budget) {
    NS_ABORT_MSG_IF(m_started || !std::isfinite(budget) || budget<=0,"invalid destination physical ceiling");
    m_destinationBudgets[ip]=budget;
}
DciGatewayNode::Resource& DciGatewayNode::QueueResource(uint32_t port,uint32_t pg) {
    auto result=m_resources.emplace(std::make_pair(port,pg),Resource{});
    auto& r=result.first->second;
    if (result.second) {
        r.id=m_nextResource++; r.port=port; r.pg=pg; r.sample=NowNs();
        r.pause=Port(port)->GetQueuePauseTime(pg).GetNanoSeconds();
    }
    return r;
}
void DciGatewayNode::Start() {
    NS_ABORT_MSG_IF(!m_enabled || m_started || !m_frame || !CustomHeader::qpnEnabled,
                    "R5 start requires peer, frame size and QPN wire format");
    NS_ABORT_MSG_IF(GetNDevices()>256,"R5 port resource bound exceeded");
    for (uint32_t i=0;i<GetNDevices();++i) {
        auto d=DynamicCast<QbbNetDevice>(GetDevice(i)); if (!d) continue;
        NS_ABORT_MSG_IF(d->GetMtu()<H().GetSerializedSize()+20,"R5 control packet exceeds MTU");
        if (i==m_wan) {
            d->m_groupClassifier=MakeBoundCallback(&DciGatewayNode::Classify,this,i);
            d->m_beforeTransmit=MakeCallback(&DciGatewayNode::Stamp,this);
        } else for (uint32_t pg=0;pg<8;++pg) QueueResource(i,pg);
        d->TraceConnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,this,i));
        d->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,this,i));
        d->TraceConnectWithoutContext("QbbPauseState",MakeBoundCallback(&DciGatewayNode::PauseChanged,this,i));
        d->m_traceTransmitResult.ConnectWithoutContext(MakeBoundCallback(&DciGatewayNode::Transmitted,this,i));
    }
    m_started=true; Event("START",0,0,"empty_flow_table");
    m_tick=Simulator::Schedule(Seconds(m_config.control),&DciGatewayNode::Tick,this);
    m_report=Simulator::Schedule(Seconds(m_config.period),&DciGatewayNode::PeriodicReport,this);
    Probe();
}
DciGatewayNode::Flow* DciGatewayNode::Match(Ptr<const Packet> p) {
    auto h=Parse(p); if (h.l3Prot!=0x11) return nullptr;
    auto it=m_data.find({h.sip,h.dip,h.udp.destQpn});
    return it==m_data.end() ? nullptr : &m_flows.at(it->second);
}
bool DciGatewayNode::BindQueue(Flow& f) {
    if (f.queue>=8) return true;
    std::set<uint32_t> used;
    for (const auto& e:m_flows) if (e.second.source && e.second.queue>=8) used.insert(e.second.queue);
    for (uint32_t queue=8;queue<BEgressQueue::fCnt;++queue) if (!used.count(queue)) {
        f.queue=queue; Port(m_wan)->BindInjectionQueue(queue,f.pg,f.id); return true;
    }
    return false;
}
void DciGatewayNode::ReleaseEmptyQueues() {
    for (auto& e:m_flows) {
        auto& f=e.second;
        if (f.source && !f.Queue() && !f.target && f.queue>=8) {
            NS_ABORT_MSG_IF(!Port(m_wan)->DetachInjectionQueue(f.queue),"cannot release empty flow queue");
            f.queue=0; Event("IDLE",f.id,f.generation,"state_and_tokens_retained");
        }
    }
}
bool DciGatewayNode::CanRetire(const Flow& f) const {
    if (!f.source || !f.completed || !f.have || f.Queue() || !f.packets.empty() ||
        f.in!=f.finalWireBytes || f.tx!=f.in || f.failures ||
        f.state[H::ARRIVED]!=f.tx || f.state[H::DEPARTED]!=f.tx || f.state[H::QUEUE] ||
        f.history.txSnapshots.empty() || f.history.txSnapshots.back().cumulativeBytes!=f.tx) return false;
    // No timeout is evidence of delivery. Require actual B counters, then retain
    // history until neither the current resource replay nor a frozen forecast uses it.
    auto resource=m_remoteResources.find(f.resource);
    if (resource==m_remoteResources.end() || int64_t(resource->second[H::SAMPLE])<=f.lastMappedArrival) return false;
    auto pending=m_resourceForecasts.find(f.resource);
    if (pending!=m_resourceForecasts.end()) for (const auto& forecast:pending->second)
        if (forecast.sample<=f.lastMappedArrival) return false;
    return true;
}
void DciGatewayNode::EraseFlow(uint32_t id) {
    auto& f=m_flows.at(id);
    NS_ABORT_MSG_IF(f.Queue() || !f.packets.empty(),"cannot reclaim queued flow");
    if (f.source) {
        if (f.queue>=8) NS_ABORT_MSG_IF(!Port(m_wan)->DetachInjectionQueue(f.queue),"reclaim attached queue");
        Port(m_wan)->RemoveInjectionGroup(id);
        m_historyCount-=f.history.txSnapshots.size();
        m_retiredHistoryFloor[f.resource]=std::max(m_retiredHistoryFloor[f.resource],f.lastMappedArrival);
    }
    if (m_retired.is_open()) { WriteFlowMetadata(m_retired,f,true); m_retired<<'\n'; m_retired.flush(); }
    Event("RETIRE",id,f.generation,"completed_and_settled"); ++m_retiredCount;
    m_registrations.erase(f.key); m_data.erase(f.key); m_flows.erase(id);
}
void DciGatewayNode::ReclaimFlows() {
    std::vector<uint32_t> retired;
    for (auto& item:m_flows) {
        auto& f=item.second;
        if (!CanRetire(f)) continue;
        if (f.retireAck) { retired.push_back(f.id); continue; }
        if (f.retireSent>=0 && NowNs()-f.retireSent<Ns(m_config.queryRetry)) continue;
        H h; h.type=H::RETIRE; h.v=FlowRecord(f);
        h.v[H::GEN]=f.remoteGeneration; h.v[H::PEER_GEN]=f.generation;
        h.v[H::ARRIVED]=f.tx; h.v[H::DEPARTED]=f.tx;
        Send(h,ControlAddress(m_peer)); f.retireSent=NowNs();
    }
    for (auto id:retired) EraseFlow(id);
}
bool DciGatewayNode::PrepareDataAdmission(Ptr<Packet> p,CustomHeader& h,uint32_t ingress,uint32_t egress,uint32_t pg) {
    if (!m_started || h.l3Prot!=0x11 || (ingress!=m_wan && egress!=m_wan)) return true;
    const bool source=egress==m_wan;
    auto reject=[&](const char* reason) {
        ++m_resourceDrops; Event("DATA_DROP",0,p->GetSize(),reason); return false;
    };
    if (!h.udp.destQpn || h.udp.destQpn>0xffffff || !pg || pg>=8 || p->GetSize()>m_frame)
        return reject("invalid_qpn_pg_or_frame");
    auto f=Match(p);
    if (!f) {
        ReleaseEmptyQueues();
        if (m_flows.size()>=m_config.maxFlows) return reject("identity_full_no_pending_buffer");
        if (source) {
            size_t used=0; for (const auto& e:m_flows) if (e.second.source && e.second.queue>=8) ++used;
            if (used>=BEgressQueue::fCnt-8) return reject("queue_full_no_pending_buffer");
        }
        NS_ABORT_MSG_IF(m_nextFlow==UINT32_MAX,"R5 generation exhausted");
        Flow record; record.key={h.sip,h.dip,h.udp.destQpn}; record.id=m_nextFlow++;
        record.generation=record.id; record.source=source; record.port=egress; record.pg=pg;
        record.sport=h.udp.sport; record.dport=h.udp.dport; record.created=record.lastData=NowNs(); record.supplySample=NowNs();
        if (!source) {
            auto& r=QueueResource(egress,pg); record.resource=r.id; record.portResource=0x80000000u|egress;
        }
        m_data.emplace(record.key,record.id); f=&m_flows.emplace(record.id,std::move(record)).first->second;
        if (source) {
            f->history.Add(NowNs(),0,0); ++m_historyCount;
            Port(m_wan)->ConfigureInjectionGroup(f->id,4*m_frame);
            NS_ABORT_MSG_IF(!BindQueue(*f),"queue reservation changed inside admission event");
        }
        Event("CREATE",f->id,f->generation,source ? "first_data_A" : "first_data_B");
    }
    if (f->source!=source || f->port!=egress || f->pg!=pg) return reject("unsupported_route_or_pg_migration");
    if (source && !BindQueue(*f)) return reject("queue_full_existing_identity");
    InterfaceTag tag;
    if (source && ingress!=m_wan && ingress<GetNDevices() &&
        p->PeekPacketTag(tag) && tag.GetPortId()==ingress &&
        f->sport==h.udp.sport && f->dport==h.udp.dport && h.GetIpv4EcnBits()==CustomHeader::ECN_CE) {
        ++m_sourceIngressCe;
        HandleFastCnp(*f,p,h,true);
    }
    f->lastData=NowNs();
    return true;
}
bool DciGatewayNode::HandleLocalCongestion(uint32_t port,uint32_t pg,Ptr<Packet> p,CustomHeader& h) {
    if (!m_started || port!=m_wan || h.l3Prot!=0x11) return false;
    InterfaceTag ingress;
    auto f=Match(p);
    if (!p->PeekPacketTag(ingress) || ingress.GetPortId()==m_wan || ingress.GetPortId()>=GetNDevices() ||
        !f || !f->source || f->port!=port || f->pg!=pg || f->sport!=h.udp.sport || f->dport!=h.udp.dport) return false;
    ++m_fastCnpCandidates;
    return HandleFastCnp(*f,p,h,false);
}
bool DciGatewayNode::HandleFastCnp(Flow& f,Ptr<Packet> p,CustomHeader& h,bool upstreamCe) {
    auto registration=m_registrations.find(f.key);
    // A successful Fast CNP covers this QP for [lastCnp, lastCnp+cnpInterval).
    // Reusing coverage never extends it. Native CNPs cannot open this window.
    const bool covered=!f.completed && registration!=m_registrations.end() &&
        registration->second.confirmed && registration->second.fastCnp &&
        f.lastCnp>=0 && NowNs()-f.lastCnp<Ns(m_config.cnpInterval);
    if (covered) {
        if (upstreamCe) ++m_fastCnpIngressCoalesced; else ++m_fastCnpCoalesced;
    } else if (!SendNearCnp(f,upstreamCe ? "upstream_ce" : "local_ecn")) return false;
    if (upstreamCe) ++m_fastCnpIngressHandled; else ++m_fastCnpReplaced;
    if (h.GetIpv4EcnBits()==CustomHeader::ECN_CE) {
        // This tree models 14 L2 bytes, but PppHeader serializes only the first two.
        // Preserve the remaining bytes when rebuilding the IPv4 checksum/header.
        std::vector<uint8_t> link(PppHeader::GetStaticSize()); p->CopyData(link.data(),link.size());
        PppHeader ppp; Ipv4Header ip;
        if (Node::ChecksumEnabled() || h.m_checksum) ip.EnableChecksum();
        p->RemoveHeader(ppp); p->RemoveHeader(ip);
        ip.SetEcn(Ipv4Header::ECN_ECT0);
        p->AddHeader(ip); p->AddHeader(ppp);
        std::copy(link.begin(),link.end(),p->GetBuffer());
        h.m_tos=ip.GetTos(); h.m_checksum=Parse(p).m_checksum;
        ++m_sourceClearedCe;
    }
    return true; // Both upstream CE and local ECN share this QP's fixed window.
}
uint32_t DciGatewayNode::Classify(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t pg) {
    auto f=self->Match(p);
    if (Parse(p).l3Prot==0x11) NS_ABORT_MSG_IF(!f || !f->source || f->port!=port || f->queue<8,"unadmitted WAN data");
    return f && f->source ? f->queue : pg;
}
void DciGatewayNode::Admitted(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t,uint32_t pg) {
    auto f=self->Match(p);
    if (f && f->port==port) {
        Add(f->in,p->GetSize()); if (f->source) f->packets.push_back(p->GetSize());
        self->PacketEvent(*f,"ADMIT",p->GetSize());
        if (f->source && f->Queue()==p->GetSize()) {
            self->RequestControl(ARRIVAL);
            if (!f->target && self->m_reallocate.IsExpired())
                self->m_reallocate=Simulator::ScheduleNow(&DciGatewayNode::ReallocateSources,self);
        }
        if (f->in==p->GetSize() && !f->source) {
            // Sample after admission finishes updating physical counters.
            Simulator::Cancel(self->m_serviceReport);
            self->m_serviceReport=Simulator::ScheduleNow(&DciGatewayNode::SampleReceiver,self);
        }
    }
    if (port!=self->m_wan) {
        auto& r=self->QueueResource(port,pg); Add(r.in,p->GetSize());
        if (!f || f->source || f->port!=port) Add(r.other,p->GetSize());
    }
}
void DciGatewayNode::Dequeue(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t pg) {
    auto f=self->Match(p);
    if (f && f->port==port) {
        Add(f->dequeued,p->GetSize()); NS_ABORT_MSG_IF(f->dequeued>f->in,"R5 queue conservation");
        if (f->source) {
            NS_ABORT_MSG_IF(f->packets.empty() || f->packets.front()!=p->GetSize(),"R5 packet FIFO mismatch");
            f->packets.pop_front();
        }
        self->PacketEvent(*f,"DEQUEUE",p->GetSize());
    }
    if (port!=self->m_wan) {
        auto& r=self->QueueResource(port,pg); Add(r.out,p->GetSize());
        if (!self->Port(port)->GetQueue()->GetNBytes(pg)) r.emptied=true;
    }
}
void DciGatewayNode::Transmitted(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,bool success) {
    auto h=Parse(p);
    if (h.l3Prot==0xf9 && success) Add(self->m_controlTx,p->GetSize());
    if (h.l3Prot==0xf9 && h.sip==ControlAddress(self->GetId()).Get()) {
        auto copy=p->Copy(); copy->RemoveAtStart(h.GetSerializedSize()); H control;
        if (copy->RemoveHeader(control) && control.type==H::FAST_CNP) {
            if (success) ++self->m_fastCnpTx; else ++self->m_fastCnpTxFailed;
        }
    }
    auto f=self->Match(p); if (!f || f->port!=port) return;
    int64_t arrival=-1;
    if (success) {
        Add(f->tx,p->GetSize());
        if (f->source) {
            arrival=NowNs()+self->m_forwardBase+self->Port(port)->GetDataRate().CalculateBytesTxTime(p->GetSize()).GetNanoSeconds();
        }
    } else Add(f->failures,p->GetSize());
    self->PacketEvent(*f,success ? "TX" : "TX_FAILED",p->GetSize(),arrival);
    if (f->source && !f->Queue()) self->RequestControl(EMPTY);
}
void DciGatewayNode::NotifyAdmissionDrop(uint32_t port,uint32_t pg,Ptr<const Packet> p) {
    if (!m_started) return;
    auto f=Match(p); if (f) { Add(f->drops,p->GetSize()); PacketEvent(*f,"DROP",p->GetSize()); }
    if (port!=m_wan) Add(QueueResource(port,pg).dropped,p->GetSize());
}
void DciGatewayNode::PauseChanged(DciGatewayNode* self,uint32_t port,uint32_t,bool) {
    if (port==self->m_wan) self->RequestControl(PAUSE);
    else if (self->m_serviceReport.IsExpired()) self->m_serviceReport=Simulator::Schedule(
        Seconds(self->m_config.control),&DciGatewayNode::SampleReceiver,self);
}
void DciGatewayNode::Event(const char* event,uint32_t id,double value,const char* reason) {
    m_events<<NowNs()<<','<<GetId()<<','<<++m_ordinal<<','<<event<<','<<id<<','<<std::setprecision(17)<<value<<','<<reason<<'\n';
}
void DciGatewayNode::PacketEvent(Flow& f,const char* event,uint32_t bytes,int64_t arrival) {
    m_packets<<NowNs()<<','<<GetId()<<','<<++m_ordinal<<','<<event<<','<<(f.source ? 'A' : 'B')<<','
        <<f.id<<','<<f.generation<<','<<f.key.srcIp<<','<<f.key.dstIp<<','<<f.key.destQpn<<','<<bytes<<','
        <<f.in<<','<<f.dequeued<<','<<f.tx<<','<<f.Queue()<<','<<arrival<<'\n';
}
Ptr<Packet> DciGatewayNode::ControlPacket(H h,Ipv4Address src,Ipv4Address dst) {
    auto packet=Create<Packet>(); packet->AddHeader(h);
    Ipv4Header ip; ip.SetSource(src); ip.SetDestination(dst); ip.SetProtocol(0xf9);
    ip.SetTtl(64); ip.SetPayloadSize(packet->GetSize()); packet->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); packet->AddHeader(ppp); return packet;
}
bool DciGatewayNode::Send(H h,Ipv4Address destination) {
    h.v[H::SESSION]=m_config.session;
    auto packet=ControlPacket(h,ControlAddress(GetId()),destination);
    auto header=Parse(packet); int egress=LookupOutputPort(packet,header);
    if (h.type==H::FAST_CNP && egress<0) return false;
    NS_ABORT_MSG_IF(egress<0,"missing R5 control route");
    const auto now=NowNs();
    m_controlTokens=std::min(double(m_config.controlBurstBytes),m_controlTokens+
        Port(m_wan)->GetDataRate().GetBitRate()/8.0*m_config.controlFraction*(now-m_controlUpdated)*1e-9);
    m_controlUpdated=now;
    if (m_controlTokens<packet->GetSize() || Port(egress)->GetQueue()->GetNBytes(0)+packet->GetSize()>m_config.controlQueueBytes) {
        ++m_controlDropped; Event("CONTROL_DROP",h.type,packet->GetSize(),"control_budget"); return false;
    }
    if (h.type==H::FAST_CNP) {
        if (!TrySendNetworkControl(packet)) return false;
    } else SendNetworkControl(packet);
    m_controlTokens-=packet->GetSize();
    return true;
}
void DciGatewayNode::Stamp(Ptr<Packet> p) {
    auto ip=Parse(p); if (ip.l3Prot!=0xf9 || ip.sip!=ControlAddress(GetId()).Get()) return;
    PppHeader ppp; Ipv4Header ipv4; H h;
    p->RemoveHeader(ppp); p->RemoveHeader(ipv4);
    NS_ABORT_MSG_IF(!p->RemoveHeader(h),"invalid locally generated R5 message");
    h.v[H::SEND]=NowNs(); h.v[H::LINK_RATE]=Port(m_wan)->GetDataRate().GetBitRate();
    p->AddHeader(h); p->AddHeader(ipv4); p->AddHeader(ppp);
}
void DciGatewayNode::Probe() {
    H h; h.type=H::PROBE; h.v[H::SEQ]=++m_probe; h.v[H::SAMPLE]=NowNs();
    Send(h,ControlAddress(m_peer));
    m_probeEvent=Simulator::Schedule(Seconds(m_config.probePeriod),&DciGatewayNode::Probe,this);
}
DciGatewayNode::Record DciGatewayNode::FlowRecord(Flow& f) {
    Record v{}; v[H::SIP]=f.key.srcIp; v[H::DIP]=f.key.dstIp; v[H::QPN]=f.key.destQpn;
    v[H::GEN]=f.generation; v[H::PEER_GEN]=f.remoteGeneration;
    v[H::SEQ]=++f.seq; v[H::SAMPLE]=NowNs(); v[H::QUEUE]=f.Queue();
    v[H::ARRIVED]=f.in; v[H::DEPARTED]=f.dequeued; v[H::DROPPED]=f.drops;
    v[H::RESOURCE]=f.resource; v[H::PORT_RESOURCE]=f.portResource; v[H::ASSOCIATION]=1;
    v[H::SERVICE]=uint64_t(f.service); v[H::BACKGROUND]=uint64_t(f.supply);
    return v;
}
bool DciGatewayNode::SwitchReceiveFromDevice(Ptr<NetDevice> dev,Ptr<Packet> p,CustomHeader& h) {
    if (m_started) {
        if (h.l3Prot==0xf9 && h.dip==ControlAddress(GetId()).Get()) { ReceiveControl(p,h); return true; }
        bool cnp=h.l3Prot==0xff && h.cnp.ecnBits;
        bool ack=(h.l3Prot==0xfc || h.l3Prot==0xfd) && (h.ack.flags&(1<<qbbHeader::FLAG_CNP));
        if (cnp || ack) for (auto& entry:m_flows) {
            auto& f=entry.second;
            auto reg=m_registrations.find(f.key);
            if (f.source && reg!=m_registrations.end() && h.sip==f.key.dstIp && h.dip==f.key.srcIp &&
                (cnp ? h.cnp.destQpn : h.ack.destQpn)==reg->second.sourceQpn) {
                ++f.nativeCnps; f.lastNative=NowNs(); Event("NATIVE_CNP",f.id,f.nativeCnps);
            }
        }
    }
    return SwitchNode::SwitchReceiveFromDevice(dev,p,h);
}
void DciGatewayNode::ReceiveControl(Ptr<Packet> p,const CustomHeader& ip) {
    Add(m_controlRx,p->GetSize()); auto copy=p->Copy(); copy->RemoveAtStart(ip.GetSerializedSize());
    H h; auto reject=[&](const char* why) { ++m_rejected; Event("REJECT",0,m_rejected,why); };
    if (!copy->RemoveHeader(h)) { reject("wire_format"); return; }
    const auto& v=h.v;
    if (v[H::SIP]>UINT32_MAX || v[H::DIP]>UINT32_MAX || v[H::QPN]>0xffffff ||
        v[H::RESOURCE]>UINT32_MAX || v[H::PORT_RESOURCE]>UINT32_MAX) { reject("field_range"); return; }
    FlowKey key{uint32_t(v[H::SIP]),uint32_t(v[H::DIP]),uint32_t(v[H::QPN])};
    if (h.type==H::COMPLETE) {
        if (ip.sip!=key.srcIp || v[H::SESSION]!=m_config.session || !key.destQpn ||
            !v[H::SRC_QPN] || v[H::SRC_QPN]>0xffffff || !v[H::CONNECTION] ||
            v[H::SAMPLE]>uint64_t(NowNs()) || NowNs()-int64_t(v[H::SAMPLE])>Ns(m_config.timeout)) {
            reject("completion_identity_or_age"); return;
        }
        auto registration=m_registrations.find(key);
        if (registration!=m_registrations.end() && (registration->second.sourceQpn!=v[H::SRC_QPN] ||
            registration->second.connection!=v[H::CONNECTION])) { reject("completion_connection"); return; }
        auto local=m_data.find(key);
        if (local!=m_data.end()) {
            auto& f=m_flows.at(local->second);
            if (!f.source || v[H::ARRIVED]<f.in || (f.completed && f.finalWireBytes!=v[H::ARRIVED])) {
                reject("completion_counters"); return;
            }
            f.completed=true; f.finalWireBytes=v[H::ARRIVED];
            Event("COMPLETE",f.id,f.finalWireBytes,"source_final_ack");
        }
        // Idempotent after retirement. An unknown completion never creates a flow.
        H ack=h; ack.type=H::COMPLETE_ACK; Send(ack,Ipv4Address(key.srcIp)); return;
    }
    if (h.type==H::REGISTER || h.type==H::REGISTER_CONFIRM) {
        if (ip.sip!=key.srcIp || !key.destQpn || !v[H::SRC_QPN] || v[H::SRC_QPN]>0xffffff ||
            !v[H::CONNECTION] || !v[H::QUERY_ID]) { reject("source_registration"); return; }
        auto it=m_registrations.find(key);
        if (h.type==H::REGISTER) {
            if (it==m_registrations.end() && m_registrations.size()>=m_config.maxFlows) { reject("registration_full"); return; }
            if (it!=m_registrations.end() && (it->second.sourceQpn!=v[H::SRC_QPN] ||
                it->second.connection!=v[H::CONNECTION] || v[H::QUERY_ID]<it->second.query)) { reject("registration_conflict"); return; }
            auto& r=m_registrations[key]; r.sourceQpn=v[H::SRC_QPN]; r.connection=v[H::CONNECTION];
            r.query=v[H::QUERY_ID]; r.ackSent=NowNs();
            H reply=h; reply.type=H::REGISTER_ACK; reply.v[H::SAMPLE]=NowNs();
            Send(reply,Ipv4Address(key.srcIp)); Event("REGISTER",0,r.sourceQpn,"no_data_state_created");
        } else {
            if (v[H::SESSION]!=m_config.session || it==m_registrations.end() || it->second.sourceQpn!=v[H::SRC_QPN] ||
                it->second.connection!=v[H::CONNECTION] || it->second.query!=v[H::QUERY_ID] ||
                v[H::SAMPLE]!=uint64_t(it->second.ackSent) || NowNs()<it->second.ackSent) { reject("registration_confirmation"); return; }
            // ACK departure preparation -> source receives/echoes -> A receives.
            // Includes local queue/serialization/processing; response timer is added separately.
            it->second.localRtt=(NowNs()-it->second.ackSent)*1e-9;
            // The source reports its configured RNIC reaction timer in the confirmation.
            if (v[H::INTERVAL_NS]>uint64_t(Ns(m_config.timeout))) { reject("host_response_timer"); return; }
            it->second.response=it->second.localRtt+v[H::INTERVAL_NS]*1e-9;
            it->second.fastCnp=(v[H::FLAGS]&H::FAST_CNP_CAPABLE)!=0;
            it->second.confirmed=true;
            Event("REGISTER_CONFIRMED",0,it->second.response,"measured_local_round_trip");
        }
        return;
    }
    if (ip.sip!=ControlAddress(m_peer).Get() || v[H::SESSION]!=m_config.peerSession ||
        v[H::SAMPLE]>uint64_t(NowNs()) || NowNs()-int64_t(v[H::SAMPLE])>Ns(m_config.timeout)) {
        reject("peer_session_or_age"); return;
    }
    if (h.type==H::RETIRE || h.type==H::RETIRE_ACK) {
        if (!key.destQpn || !v[H::GEN] || !v[H::PEER_GEN]) { reject("retire_identity"); return; }
        auto local=m_data.find(key);
        if (h.type==H::RETIRE) {
            if (local!=m_data.end()) {
                auto& f=m_flows.at(local->second);
                if (f.source || f.generation!=v[H::GEN] || f.Queue() || f.failures ||
                    f.in!=v[H::ARRIVED] || f.tx!=f.in) { reject("retire_unsettled_receiver"); return; }
                f.remoteGeneration=v[H::PEER_GEN]; EraseFlow(f.id);
            }
            // Repeated requests are acknowledged even when the first ACK was lost.
            H ack=h; ack.type=H::RETIRE_ACK; ack.v[H::SAMPLE]=NowNs();
            Send(ack,ControlAddress(m_peer));
        } else if (local!=m_data.end()) {
            auto& f=m_flows.at(local->second);
            if (!f.source || !f.completed || f.generation!=v[H::PEER_GEN] ||
                f.remoteGeneration!=v[H::GEN] || f.tx!=v[H::ARRIVED] || f.retireSent<0) {
                reject("retire_ack_identity"); return;
            }
            f.retireAck=true;
        }
        return;
    }
    auto observeReverseDelay=[&]() {
      if (v[H::LINK_RATE] && v[H::SEND]<=uint64_t(NowNs())) {
        auto serialization=DataRate(v[H::LINK_RATE]).CalculateBytesTxTime(p->GetSize()).GetNanoSeconds();
        auto base=NowNs()-int64_t(v[H::SEND])-serialization;
        if (base>=0 && base<=Ns(m_config.timeout)) m_reverseBase=base;
      }
    };
    if (h.type==H::PROBE) {
        if (!v[H::SEQ] || !v[H::LINK_RATE]) { reject("probe"); return; }
        observeReverseDelay();
        H reply; reply.type=H::PROBE_REPLY; reply.v[H::SEQ]=v[H::SEQ]; reply.v[H::SAMPLE]=NowNs();
        reply.v[H::BASE_DELAY_NS]=m_reverseBase; Send(reply,ControlAddress(m_peer)); return;
    }
    if (h.type==H::PROBE_REPLY) {
        if (v[H::SEQ]<=m_lastProbeReply || v[H::SEQ]>m_probe || v[H::BASE_DELAY_NS]>uint64_t(Ns(m_config.timeout))) {
            reject("probe_order"); return;
        }
        observeReverseDelay(); m_lastProbeReply=v[H::SEQ]; m_forwardBase=v[H::BASE_DELAY_NS];
        Event("DELAY",0,m_forwardBase,"measured_forward_base_ns"); return;
    }
    if (h.type==H::RESOURCE_STATE) {
        if (v[H::RESOURCE]==0) {
            if (v[H::SEQ]<=m_pressureSequence) { reject("pressure_sequence"); return; }
            m_pressureSequence=v[H::SEQ]; m_peerFull=v[H::FLAGS]&H::FULL;
            m_lastPeerPressure=v[H::SAMPLE]; observeReverseDelay(); RequestControl(PRESSURE); return;
        }
        auto old=m_remoteResources.find(v[H::RESOURCE]);
        if (!v[H::SEQ] || !v[H::CAPACITY] || v[H::SERVICE]>v[H::CAPACITY] || v[H::BUFFER]>m_config.bufferBytes ||
            v[H::DEPARTED]>v[H::ARRIVED] || v[H::QUEUE]!=v[H::ARRIVED]-v[H::DEPARTED] ||
            (old==m_remoteResources.end() && m_remoteResources.size()>=2305) ||
            (old!=m_remoteResources.end() && (v[H::SEQ]<=old->second[H::SEQ] || v[H::SAMPLE]<old->second[H::SAMPLE]))) {
            reject("resource_state"); return;
        }
        auto pending=m_resourceForecasts.find(v[H::RESOURCE]);
        if (pending!=m_resourceForecasts.end() && !pending->second.empty())
            m_validationSamples.emplace_back(v[H::RESOURCE],v);
        m_remoteResources[v[H::RESOURCE]]=v; observeReverseDelay();
        if ((v[H::FLAGS]&H::PAUSED) || !v[H::SERVICE]) StopResource(v[H::RESOURCE]);
        RequestControl(RESOURCE_FEEDBACK); return;
    }
    auto it=m_data.find(key);
    if (it==m_data.end()) { Event("STATE_IGNORED",0,v[H::SEQ],"no_live_identity"); return; }
    auto& f=m_flows.at(it->second);
    if (!f.source) { reject("flow_role"); return; }
    // B pushes observations for its independently discovered flow. No query or
    // echoed A generation is needed: only live identities accept observations.
    if (h.type!=H::FLOW_STATE || !v[H::GEN] || !v[H::SEQ] || !v[H::RESOURCE] || !v[H::PORT_RESOURCE] ||
        v[H::SAMPLE]<uint64_t(f.created) || v[H::DEPARTED]>v[H::ARRIVED] ||
        v[H::QUEUE]!=v[H::ARRIVED]-v[H::DEPARTED] ||
        (f.have && (v[H::GEN]!=f.remoteGeneration || v[H::SEQ]<=f.state[H::SEQ] ||
          v[H::SAMPLE]<f.state[H::SAMPLE] || v[H::ARRIVED]<f.state[H::ARRIVED] ||
          v[H::DEPARTED]<f.state[H::DEPARTED] || v[H::DROPPED]<f.state[H::DROPPED] ||
          v[H::RESOURCE]!=f.resource || v[H::PORT_RESOURCE]!=f.portResource))) { reject("flow_generation_sequence_or_counters"); return; }
    f.have=true; f.remoteGeneration=v[H::GEN]; f.resource=v[H::RESOURCE]; f.portResource=v[H::PORT_RESOURCE];
    observeReverseDelay(); f.state=v; Event("STATE",f.id,v[H::SEQ]); RequestControl(FLOW_FEEDBACK);
}
void DciGatewayNode::ValidateForecasts(uint32_t resource,const Record& v) {
    // Validate only exact sample times. Replayed actual input never overwrites a frozen plan.
    auto found=m_resourceForecasts.find(resource);
    if (found==m_resourceForecasts.end() || found->second.empty() ||
        found->second.front().end>int64_t(v[H::SAMPLE])) return;
    auto& forecasts=found->second;
    std::vector<Arrival> actual;
    auto retired=m_retiredHistoryFloor.find(resource);
    int64_t floor=retired==m_retiredHistoryFloor.end() ? 0 : retired->second;
    bool haveMembers=retired!=m_retiredHistoryFloor.end();
    for (const auto& item:m_flows) {
        const auto& f=item.second;
        if (!f.source || !f.have || f.resource!=resource) continue;
        haveMembers=true; floor=std::max(floor,f.history.floor);
        auto events=Arrivals(f); actual.insert(actual.end(),events.begin(),events.end());
    }
    std::stable_sort(actual.begin(),actual.end(),[](const Arrival& a,const Arrival& b){return a.time<b.time;});
    std::map<std::tuple<int64_t,int64_t,double,double>,double> replayed;
    while (!forecasts.empty() && forecasts.front().end<=int64_t(v[H::SAMPLE])) {
        auto prediction=forecasts.front(); forecasts.pop_front();
        const char* reason=prediction.end!=int64_t(v[H::SAMPLE]) ? "missing_aligned_sample" :
            (!haveMembers || floor>prediction.sample) ? "history_expired" : "exact_time_conditional";
        bool valid=prediction.end==int64_t(v[H::SAMPLE]) && haveMembers && floor<=prediction.sample;
        if (valid) ++m_forecastValidated; else ++m_forecastUnavailable;
        double replay=0;
        if (valid) {
            auto key=std::make_tuple(prediction.sample,prediction.end,prediction.initial,prediction.service);
            auto cached=replayed.find(key);
            if (cached==replayed.end()) cached=replayed.emplace(key,
                Replay(prediction.sample,prediction.end,prediction.decision,prediction.initial,prediction.service,actual).queue).first;
            replay=cached->second;
        }
        m_predictions<<NowNs()<<','<<GetId()<<','<<prediction.id<<','<<resource<<",VALIDATE,"
            <<prediction.sample<<','<<prediction.end<<','<<prediction.initial<<','
            <<prediction.service<<','<<prediction.predicted<<",0,"<<prediction.planned<<','<<v[H::QUEUE]<<','
            <<replay<<','<<reason<<'\n';
    }
}
bool DciGatewayNode::RecordForecast(uint32_t resource,const Forecast& prediction) {
    auto& forecasts=m_resourceForecasts[resource];
    if (forecasts.size()>=m_config.maxForecasts) {
        ++m_forecastRejected;
        Event("FORECAST_MISSING",resource,prediction.id,"bounded_resource_pool_new_record");
        return false; // Never evict or overwrite an outstanding frozen prediction.
    }
    if (forecasts.empty() || forecasts.back().end<=prediction.end) forecasts.push_back(prediction);
    else forecasts.insert(std::upper_bound(forecasts.begin(),forecasts.end(),prediction.end,
        [](int64_t end,const Forecast& p) { return end<p.end; }),prediction);
    ++m_forecastCreated; m_forecastPeak=std::max(m_forecastPeak,uint64_t(forecasts.size()));
    return true;
}
void DciGatewayNode::MaintainHistory(Flow& f,int64_t now) {
    // A-time retention covers the oldest accepted B sample even under the
    // largest admissible probe delay. Validation never extends this window.
    auto delay=std::max(Ns(m_config.initialDelay),Ns(m_config.timeout))+
        Port(m_wan)->GetDataRate().CalculateBytesTxTime(m_frame).GetNanoSeconds();
    auto before=std::max(int64_t(0),now-Ns(m_config.timeout)-delay-Ns(m_config.control));
    auto pending=m_resourceForecasts.find(f.resource);
    if (pending!=m_resourceForecasts.end()) {
        auto& forecasts=pending->second;
        while (!forecasts.empty() && now-forecasts.front().end>Ns(m_config.timeout)) {
            ++m_forecastExpired;
            Event("FORECAST_MISSING",f.resource,forecasts.front().id,"feedback_timeout"); forecasts.pop_front();
        }
    }
    m_historyCount-=f.history.Prune(before);
}
void DciGatewayNode::SampleReceiver() {
    Simulator::Cancel(m_serviceReport);
    const auto now=NowNs(); uint64_t poolQueue=0;
    // A PFC-triggered sample can coincide with the periodic sample. Publish
    // one snapshot/pressure sequence per time instant, not two identical ones.
    if (now<=m_lastReceiverSample) return;
    m_lastReceiverSample=now;
    for (uint32_t port=0;port<GetNDevices();++port) if (auto dev=DynamicCast<QbbNetDevice>(GetDevice(port)))
        poolQueue+=dev->GetQueue()->GetNBytesTotal();
    std::map<uint32_t,uint64_t> portOther;
    for (auto& entry:m_resources) {
        auto& r=entry.second; if (now<=r.sample) continue;
        auto dev=Port(r.port); const auto elapsed=now-r.sample;
        const auto pause=dev->GetQueuePauseTime(r.pg).GetNanoSeconds();
        const auto paused=std::clamp(pause-r.pause,int64_t(0),elapsed);
        const auto active=elapsed-paused; const double capacity=dev->GetDataRate().GetBitRate()/8.0;
        bool shared=false;
        for (const auto& other:m_resources) if (other.second.port==r.port && other.second.pg!=r.pg &&
             (other.second.out>other.second.lastOut || dev->GetQueue()->GetNBytes(other.second.pg))) shared=true;
        if (!shared) { r.service=capacity; r.valid=true; }
        else if (!r.emptied && active>0) { r.service=std::min(capacity,(r.out-r.lastOut)/(active*1e-9)); r.valid=true; }
        // An empty sample measures demand, not capacity. Cold shared service stays explicitly approximate.
        if (!r.valid) {
            unsigned contenders=1;
            for (const auto& other:m_resources) if (other.second.port==r.port && other.second.pg &&
                other.second.pg!=r.pg && !dev->IsQueuePaused(other.second.pg) &&
                (other.second.in>other.second.lastOut || dev->GetQueue()->GetNBytes(other.second.pg))) ++contenders;
            // Explicit scheduler-based cold estimate; VALID remains false until observed.
            r.service=capacity/contenders;
        }
        Record v{}; v[H::RESOURCE]=r.id; v[H::PORT_RESOURCE]=0x80000000u|r.port;
        v[H::SEQ]=++r.seq; v[H::SAMPLE]=now; v[H::QUEUE]=dev->GetQueue()->GetNBytes(r.pg);
        v[H::ARRIVED]=r.in; v[H::DEPARTED]=r.out; v[H::DROPPED]=r.dropped; v[H::OTHER]=r.other;
        NS_ABORT_MSG_IF(r.out>r.in || r.in-r.out!=v[H::QUEUE],"R5 physical queue conservation");
        v[H::SERVICE]=dev->IsQueuePaused(r.pg) ? 0 : uint64_t(r.service);
        v[H::BACKGROUND]=uint64_t((r.other-r.lastOther)/(elapsed*1e-9));
        v[H::CAPACITY]=uint64_t(capacity); v[H::BUFFER]=m_config.bufferBytes; v[H::POOL_QUEUE]=poolQueue;
        v[H::UNPAUSED]=uint64_t(r.service); v[H::PAUSE_NS]=paused; v[H::INTERVAL_NS]=elapsed;
        v[H::FLAGS]=(r.valid ? H::VALID : 0)|(dev->IsQueuePaused(r.pg) ? H::PAUSED : 0)|
                    (r.emptied ? H::EMPTIED : 0)|(shared ? H::APPROXIMATE : 0);
        r.state=v; portOther[r.port]+=v[H::BACKGROUND];
        if (r.pg && r.in) {
            H h; h.type=H::RESOURCE_STATE; h.v=v; Send(h,ControlAddress(m_peer));
            m_resourceLog<<now<<','<<GetId()<<','<<r.id<<','<<r.port<<','<<r.pg<<','<<v[H::QUEUE]<<','
                <<r.in<<','<<r.out<<','<<r.other<<','<<r.dropped<<','<<v[H::SERVICE]<<','<<v[H::CAPACITY]<<','
                <<v[H::BACKGROUND]<<','<<paused<<','<<elapsed<<','<<v[H::FLAGS]<<'\n';
        }
    }
    for (const auto& item:portOther) {
        H h; h.type=H::RESOURCE_STATE; auto& v=h.v;
        v[H::RESOURCE]=0x80000000u|item.first; v[H::SEQ]=uint64_t(now)+1; v[H::SAMPLE]=now;
        v[H::CAPACITY]=Port(item.first)->GetDataRate().GetBitRate()/8;
        v[H::SERVICE]=v[H::CAPACITY]; v[H::BACKGROUND]=item.second;
        v[H::BUFFER]=m_config.bufferBytes; v[H::POOL_QUEUE]=poolQueue; v[H::FLAGS]=H::VALID|H::PORT;
        Send(h,ControlAddress(m_peer));
    }
    for (auto& item:m_resources) {
        auto& r=item.second;
        if (now<=r.sample) continue;
        r.sample=now; r.lastOut=r.out; r.lastOther=r.other;
        r.pause=Port(r.port)->GetQueuePauseTime(r.pg).GetNanoSeconds(); r.emptied=!Port(r.port)->GetQueue()->GetNBytes(r.pg);
    }
    std::vector<uint32_t> receivers;
    for (auto& item:m_flows) if (!item.second.source) {
        auto& f=item.second; receivers.push_back(f.id);
        if (now>f.supplySample) {
            const auto seconds=(now-f.supplySample)*1e-9;
            f.supply=(f.in-f.supplyIn)/seconds; f.service=(f.dequeued-f.previousTx)/seconds;
            f.supplyIn=f.in; f.previousTx=f.dequeued; f.supplySample=now;
        }
    }
    for (uint32_t i=0;i<std::min<size_t>(m_config.maxReports,receivers.size());++i) {
        auto& f=m_flows.at(receivers[(m_reportCursor+i)%receivers.size()]);
        H h; h.type=H::FLOW_STATE; h.v=FlowRecord(f); Send(h,ControlAddress(m_peer));
    }
    if (!receivers.empty()) m_reportCursor=(m_reportCursor+m_config.maxReports)%receivers.size();
    H pressure; pressure.type=H::RESOURCE_STATE; pressure.v[H::SAMPLE]=now; pressure.v[H::SEQ]=uint64_t(now)+1;
    pressure.v[H::FLAGS]=m_flows.size()>=m_config.maxFlows ? H::FULL : 0; Send(pressure,ControlAddress(m_peer));
}
void DciGatewayNode::PeriodicReport() {
    SampleReceiver();
    m_report=Simulator::Schedule(Seconds(m_config.period),&DciGatewayNode::PeriodicReport,this);
}
void DciGatewayNode::History::Add(int64_t time,uint64_t bytes,int64_t delay) {
    NS_ABORT_MSG_IF(!txSnapshots.empty() && (time<=txSnapshots.back().timeNs ||
        bytes<txSnapshots.back().cumulativeBytes),"nonmonotonic TX snapshot");
    txSnapshots.push_back({time,bytes,delay});
}
size_t DciGatewayNode::History::Prune(int64_t before) {
    size_t count=0;
    // Keep the predecessor at the boundary for differences and interpolation.
    while (txSnapshots.size()>1 && txSnapshots[1].timeNs<before) {
        floor=std::max(floor,txSnapshots[1].timeNs+txSnapshots[1].delayNs);
        txSnapshots.pop_front(); ++count;
    }
    return count;
}
std::vector<DciGatewayNode::ArrivalChange> DciGatewayNode::ArrivalChanges(
        int64_t sample,int64_t end,const std::vector<Arrival>& arrivals) {
    std::vector<ArrivalChange> changes;
    for (const auto& a:arrivals) {
        if (a.begin<0) {
            // Retain the existing conservative same-nanosecond future-packet rule.
            if (a.time>=sample && a.time<=end) changes.push_back({a.time,0,double(a.bytes)});
        } else {
            NS_ABORT_MSG_IF(a.time<=a.begin,"invalid arrival interval");
            auto begin=std::max(sample,a.begin), finish=std::min(end,a.time);
            if (begin>=finish || !a.bytes) continue;
            double rate=double(a.bytes)/(a.time-a.begin); // bytes/ns
            changes.push_back({begin,rate,0}); changes.push_back({finish,-rate,0});
        }
    }
    std::stable_sort(changes.begin(),changes.end(),[](const ArrivalChange& a,const ArrivalChange& b) {
        return a.time<b.time;
    });
    return changes;
}
DciGatewayNode::Prediction DciGatewayNode::Replay(int64_t sample,int64_t end,int64_t peakBegin,
        double queue,double service,const std::vector<Arrival>& arrivals) {
    NS_ABORT_MSG_IF(sample>end || queue<0 || !std::isfinite(queue+service),"invalid replay");
    double q=queue, peak=sample>=peakBegin ? q : 0, rate=0; auto time=sample;
    auto advance=[&](int64_t next) {
        const double net=rate-service*1e-9;
        if (time<peakBegin && next>=peakBegin)
            peak=std::max(peak,std::max(0.0,q+net*(peakBegin-time)));
        q=std::max(0.0,q+net*(next-time)); time=next;
        if (time>=peakBegin) peak=std::max(peak,q);
    };
    for (const auto& change:ArrivalChanges(sample,end,arrivals)) {
        advance(change.time); rate+=change.rate; q+=change.bytes;
        if (time>=peakBegin) peak=std::max(peak,q);
    }
    advance(end); return {q,peak};
}
std::vector<DciGatewayNode::Arrival> DciGatewayNode::Arrivals(const Flow& f) {
    const auto& snapshots=f.history.txSnapshots;
    ++m_historyCopies; m_historyCopiedRecords+=snapshots.size();
    std::vector<Arrival> result; result.reserve(snapshots.size());
    for (size_t i=1;i<snapshots.size();++i) {
        const auto& a=snapshots[i-1]; const auto& b=snapshots[i];
        if (b.cumulativeBytes>a.cumulativeBytes)
            result.push_back({b.timeNs+b.delayNs,b.cumulativeBytes-a.cumulativeBytes,a.timeNs+b.delayNs});
    }
    // Probe updates can move mapped intervals; never remap previously recorded ones.
    std::stable_sort(result.begin(),result.end(),[](const Arrival& a,const Arrival& b){return a.time<b.time;});
    return result;
}
std::map<uint32_t,double> DciGatewayNode::Allocate(const std::vector<Allocation>& flows,
        const std::map<uint64_t,double>& budgets) {
    std::map<uint32_t,double> rates; std::set<uint32_t> open;
    for (const auto& f:flows) {
        NS_ABORT_MSG_IF(!std::isfinite(f.cap) || f.cap<0,"invalid allocation cap");
        rates[f.id]=0; if (f.cap>0) open.insert(f.id);
    }
    for (size_t round=0;round<=flows.size() && !open.empty();++round) {
        double step=std::numeric_limits<double>::max();
        std::map<uint64_t,double> used; std::map<uint64_t,size_t> active;
        for (const auto& f:flows) {
            if (open.count(f.id)) step=std::min(step,std::max(0.0,f.cap-rates[f.id]));
            for (auto resource:f.resources) {
                used[resource]+=rates[f.id]; if (open.count(f.id)) ++active[resource];
            }
        }
        for (const auto& a:active) step=std::min(step,std::max(0.0,budgets.at(a.first)-used[a.first])/a.second);
        for (auto id:open) rates[id]+=step;
        std::set<uint64_t> exhausted;
        for (const auto& a:active) if (used[a.first]+a.second*step>=budgets.at(a.first)-1e-6) exhausted.insert(a.first);
        size_t removed=0;
        for (const auto& f:flows) if (open.count(f.id)) {
            bool freeze=rates[f.id]>=f.cap-1e-6;
            for (auto resource:f.resources) freeze=freeze || exhausted.count(resource);
            if (freeze) { open.erase(f.id); ++removed; }
        }
        NS_ABORT_MSG_IF(!removed,"max-min allocator failed to make progress");
    }
    return rates;
}
bool DciGatewayNode::FutureDepartures(int64_t end,std::map<uint32_t,std::vector<Arrival>>& result) {
    struct Model {
        uint32_t id, queue, burst;
        double tokens, rate, supply, generated=0;
        int64_t updated;
        const std::deque<uint32_t>* packets;
        size_t next=0;
        bool supplied=false;
        bool Empty() const { return next==packets->size() && !supplied; }
        uint32_t Head(uint32_t frame) const { return next<packets->size() ? (*packets)[next] : frame; }
    };
    ++m_futureModels;
    std::vector<Model> flows; const auto now=NowNs(); auto dev=Port(m_wan);
    const auto rate=dev->GetDataRate();
    const auto frameTime=rate.CalculateBytesTxTime(m_frame).GetNanoSeconds();
    const auto gap=dev->GetTransmitGap().GetNanoSeconds();
    for (auto& item:m_flows) {
        auto& f=item.second;
        if (!f.source || f.target<=0 || f.queue<8 || dev->IsQueuePaused(f.pg)) continue;
        auto bucket=dev->ReadInjectionSnapshot(f.id);
        flows.push_back({f.id,f.queue,bucket.burst,bucket.tokens,f.target,f.supply,0,now,&f.packets});
    }
    if (flows.empty()) return true;
    std::sort(flows.begin(),flows.end(),[](const Model& a,const Model& b) { return a.queue<b.queue; });
    uint32_t cursor=dev->GetQueue()->GetRoundRobinCursor();
    int64_t time=std::max(now,dev->GetTransmitAvailable().GetNanoSeconds())+
        dev->GetDataRate().CalculateBytesTxTime(dev->GetQueue()->GetNBytes(0)).GetNanoSeconds();
    uint32_t count=0;
    while (time<=end) {
        int64_t wake=INT64_MAX; Model* selected=nullptr;
        for (auto& model:flows) {
            double dt=(time-model.updated)*1e-9;
            model.tokens=std::min(double(model.burst),model.tokens+model.rate*dt);
            model.generated+=model.supply*dt; model.updated=time;
            if (model.Empty() && model.generated>=m_frame) {
                // New supply arrives only after its modeled interarrival time.
                model.supplied=true; model.generated-=m_frame;
            }
            uint32_t bytes=model.Head(m_frame);
            double waiting=model.Empty() ? (model.supply>0 ? std::max(0.0,(bytes-model.generated)/model.supply) : INFINITY) : 0;
            waiting=std::max(waiting,std::max(0.0,(bytes-model.tokens)/model.rate));
            if (!model.Empty() && model.tokens+1e-6>=bytes) {
                if (!selected || (model.queue>cursor && selected->queue<=cursor) ||
                    ((model.queue>cursor)==(selected->queue>cursor) && model.queue<selected->queue)) selected=&model;
            } else if (std::isfinite(waiting) && waiting<=(end-time)*1e-9)
                wake=std::min(wake,time+std::max(int64_t(1),int64_t(std::ceil(waiting*1e9))));
        }
        if (!selected) { if (wake==INT64_MAX) break; time=wake; continue; }
        if (++count>m_config.maxFuturePackets) return false;
        auto bytes=selected->Head(m_frame);
        if (selected->next<selected->packets->size()) ++selected->next; else selected->supplied=false;
        ++m_futurePackets;
        selected->tokens=std::max(0.0,selected->tokens-bytes); cursor=selected->queue;
        result[selected->id].push_back({time,bytes});
        time+=std::max(int64_t(1),(bytes==m_frame ? frameTime : rate.CalculateBytesTxTime(bytes).GetNanoSeconds())+gap);
    }
    return true;
}
void DciGatewayNode::RequestControl(Trigger trigger) {
    ++m_triggers[trigger];
    if (m_controlPending) ++m_coalesced;
    m_controlPending=true;
    // Packet arrival and transmit completion already wake the device scheduler.
    // Keep the current allocation until the next control tick; never mint credit
    // or rerun the full prediction from the packet path.
}
void DciGatewayNode::StopResource(uint32_t resource) {
    auto cached=m_resourceControls.find(resource);
    if (cached!=m_resourceControls.end()) cached->second.budget=0;
    bool changed=false;
    std::vector<QbbNetDevice::InjectionControl> controls;
    for (auto& item:m_flows) {
        auto& f=item.second; if (!f.source) continue;
        if (f.resource==resource && f.target>0) {
            f.target=0; changed=true; ++m_safetyStops;
            Event("CONTROL",f.id,0,"remote_service_stopped_immediate");
        }
        controls.push_back({f.id,f.target});
    }
    // Only decreases are allowed between decisions. Recovery and redistribution
    // use the normal predictor/allocator on the next tick.
    if (changed) Port(m_wan)->ApplyInjectionControls(controls);
}
void DciGatewayNode::AllocateSources() {
    const auto now=NowNs(); auto dev=Port(m_wan);
    const double capacity=dev->GetDataRate().GetBitRate()/8.0;
    std::map<uint64_t,double> budgets{{0,capacity}};
    std::vector<Allocation> eligible;
    for (auto& item:m_flows) {
        auto& f=item.second; if (!f.source) continue;
        f.target=0; f.candidate=capacity; f.reason="startup"; f.service=0;
        if (!f.Queue()) { f.reason="idle"; continue; }
        if (dev->IsQueuePaused(f.pg)) { f.candidate=0; f.reason="local_pfc"; }
        Allocation entry{f.id,f.candidate,{0}};
        if (!f.have && m_peerFull && now-m_lastPeerPressure<=Ns(m_config.timeout)) {
            f.candidate=0; f.reason="peer_identity_full";
        }
        if (f.have) {
            auto resource=m_remoteResources.find(f.resource), port=m_remoteResources.find(f.portResource);
            bool fresh=now-int64_t(f.state[H::SAMPLE])<=Ns(m_config.timeout) && resource!=m_remoteResources.end() &&
                port!=m_remoteResources.end() && now-int64_t(resource->second[H::SAMPLE])<=Ns(m_config.timeout) &&
                now-int64_t(port->second[H::SAMPLE])<=Ns(m_config.timeout);
            if (!fresh) {
                f.candidate=0; f.reason="stale_state";
            } else {
                const auto& p=port->second;
                auto cached=m_resourceControls.find(f.resource);
                if (cached==m_resourceControls.end() || now-cached->second.sample>Ns(m_config.timeout)) {
                    f.candidate=0; f.reason="await_resource_budget";
                    entry.cap=0; eligible.push_back(entry); continue;
                }
                const auto& control=cached->second;
                const auto& r=resource->second;
                budgets[f.resource]=std::min(control.budget,
                    std::max(0.0,double(r[H::SERVICE])-double(r[H::BACKGROUND])));
                if (r[H::FLAGS]&H::PAUSED) budgets[f.resource]=0;
                f.service=control.net; f.reason=control.reason;
                if (!budgets[f.resource]) f.candidate=0;
                budgets[f.portResource]=std::max(0.0,double(p[H::CAPACITY])-double(p[H::BACKGROUND]));
                entry.resources.push_back(f.resource); entry.resources.push_back(f.portResource);
            }
        }
        if (dev->IsQueuePaused(f.pg)) { f.candidate=0; f.reason="local_pfc"; }
        auto dest=m_destinationBudgets.find(f.key.dstIp);
        if (dest!=m_destinationBudgets.end()) {
            uint64_t resource=(uint64_t(1)<<32)|f.key.dstIp;
            budgets[resource]=dest->second; entry.resources.push_back(resource);
        }
        if (m_modelBlocked) { f.candidate=0; f.reason="future_model_bound"; }
        entry.cap=f.candidate; eligible.push_back(entry);
    }
    auto rates=Allocate(eligible,budgets);
    for (auto& item:m_flows) if (item.second.source && rates.count(item.first)) item.second.target=rates[item.first];
}
void DciGatewayNode::ReallocateSources() {
    ++m_reallocations;
    // Only reuse the last safe resource budgets. No history replay, forecasting,
    // token reset or periodic sampling is performed on the packet path.
    AllocateSources();
    std::vector<QbbNetDevice::InjectionControl> controls;
    for (const auto& item:m_flows) if (item.second.source) {
        controls.push_back({item.first,item.second.target});
        Event("REALLOCATE",item.first,item.second.target,item.second.reason);
    }
    Port(m_wan)->ApplyInjectionControls(controls);
}
void DciGatewayNode::ActivateSources() {
    Simulator::Cancel(m_reallocate);
    const auto now=NowNs(); const auto dev=Port(m_wan); ++m_decision;
    const double capacity=dev->GetDataRate().GetBitRate()/8.0;
    const int64_t df=m_forwardBase+dev->GetDataRate().CalculateBytesTxTime(m_frame).GetNanoSeconds();
    const int64_t db=m_reverseBase+dev->GetDataRate().CalculateBytesTxTime(ControlFrameBytes()).GetNanoSeconds();
    const int64_t horizon=now+df+db, effective=now+df;
    std::map<uint32_t,std::vector<uint32_t>> members;
    std::map<uint32_t,std::vector<Arrival>> histories;
    for (auto& item:m_flows) if (item.second.source) {
        auto& f=item.second; f.target=0;
        if (f.have) members[f.resource].push_back(f.id);
        histories[f.id]=Arrivals(f);
    }
    // Each source history and each resource history is materialized once per
    // decision, including idle flows whose packets are still in flight.
    m_resourceControls.clear();
    auto& resourceControls=m_resourceControls;
    auto byTime=[](const Arrival& a,const Arrival& b) { return a.time<b.time; };
    for (const auto& item:members) {
        auto r=m_remoteResources.find(item.first); if (r==m_remoteResources.end()) continue;
        const auto& state=r->second;
        auto& control=resourceControls[item.first]; control.sample=state[H::SAMPLE];
        control.net=double(state[H::SERVICE])-double(state[H::BACKGROUND]);
        std::vector<Arrival> all;
        for (auto id:item.second) {
            control.covered=control.covered && m_flows.at(id).history.Covers(state[H::SAMPLE]);
            const auto& events=histories.at(id); all.insert(all.end(),events.begin(),events.end());
        }
        std::stable_sort(all.begin(),all.end(),byTime);
        if (now-int64_t(state[H::SAMPLE])>Ns(m_config.timeout)) control.reason="stale_state";
        else if (!control.covered) control.reason="resource_history_missing";
        else {
            control.effective=Replay(state[H::SAMPLE],effective,now,state[H::QUEUE],control.net,all);
            control.risk=Replay(state[H::SAMPLE],horizon,now,state[H::QUEUE],control.net,all);
            // One queue correction and one service budget for the entire FIFO.
            // Historical per-flow input fractions never determine injection eligibility.
            control.budget=db>0 ? std::clamp(control.net+(m_config.qref-control.effective.queue)/(db*1e-9),
                                            0.0,std::max(0.0,control.net)) : 0;
            if (!control.budget) control.reason=db>0 ? "resource_queue_budget_zero" : "uncontrollable_horizon";
            if (!(state[H::FLAGS]&H::VALID)) {
                control.budget=std::min(control.budget,capacity*m_config.conservativeFraction);
                control.reason="service_approximation";
            }
            if ((state[H::FLAGS]&H::PAUSED) || !state[H::SERVICE]) {
                control.budget=0; control.reason="remote_service_stopped";
            }
            double available=std::max(0.0,double(state[H::BUFFER])-double(state[H::POOL_QUEUE])+double(state[H::QUEUE]));
            if (control.risk.peak+m_config.margin>=available) {
                control.budget=0; control.reason="unavoidable_peak";
            }
        }
    }
    m_modelBlocked=false; AllocateSources();
    std::map<uint32_t,std::vector<Arrival>> future;
    bool futureValid=FutureDepartures(now+db,future);
    m_modelBlocked=!futureValid;
    if (!futureValid) {
        for (auto& item:m_flows) if (item.second.source) { item.second.target=0; item.second.reason="future_model_bound"; }
        future.clear(); Event("MODEL_LIMIT",0,m_config.maxFuturePackets);
    }
    // Map future departures to arrivals once, then merge with sorted history.
    // The fixed propagation base and serialized non-overlapping transmissions
    // make future arrivals ordered, including short tail packets.
    std::map<uint32_t,std::vector<Arrival>> plannedFlows,resourcePlans;
    const auto linkRate=dev->GetDataRate();
    const auto frameTime=linkRate.CalculateBytesTxTime(m_frame).GetNanoSeconds();
    for (const auto& item:histories) {
        auto& all=plannedFlows[item.first]; all=item.second;
        const auto split=all.size();
        for (auto event:future[item.first]) {
            event.time+=m_forwardBase+(event.bytes==m_frame ? frameTime : linkRate.CalculateBytesTxTime(event.bytes).GetNanoSeconds());
            all.push_back(event);
        }
        std::inplace_merge(all.begin(),all.begin()+split,all.end(),byTime);
    }
    for (const auto& item:members) {
        auto& all=resourcePlans[item.first];
        for (auto id:item.second) {
            const auto& events=plannedFlows.at(id); all.insert(all.end(),events.begin(),events.end());
        }
        std::stable_sort(all.begin(),all.end(),byTime);
    }
    // D4: observe the final plan's peak, without introducing an extra iterative
    // limiter or a new shared startup/recovery permission mechanism.
    for (const auto& item:members) {
        auto resource=m_remoteResources.find(item.first); if (resource==m_remoteResources.end()) continue;
        const auto& r=resource->second;
        const auto& control=resourceControls.at(item.first);
        double allocated=0, planned=0;
        for (auto id:item.second) {
            allocated+=m_flows.at(id).target;
            for (auto event:future[id]) planned+=event.bytes;
        }
        m_budgetLog<<std::setprecision(17)<<now<<','<<GetId()<<','<<m_decision<<','<<item.first<<','
            <<r[H::SAMPLE]<<','<<r[H::QUEUE]<<','<<control.net<<','<<control.effective.queue<<','
            <<control.risk.peak<<','<<control.budget<<','<<allocated<<','<<control.covered<<','<<control.reason<<'\n';
        NS_ABORT_MSG_IF(allocated>control.budget+1e-6,"R5 shared resource budget exceeded");
        if (!control.covered || now-int64_t(r[H::SAMPLE])>Ns(m_config.timeout)) continue;
        const auto& all=resourcePlans.at(item.first);
        auto prediction=Replay(r[H::SAMPLE],horizon,now,r[H::QUEUE],control.net,all);
        Event("RESOURCE_PLAN_PEAK",item.first,prediction.peak,"model_only_no_joint_burst_guarantee");
        m_predictions<<std::setprecision(17)<<now<<','<<GetId()<<','<<m_decision<<','<<item.first
            <<",FREEZE,"<<r[H::SAMPLE]<<','<<horizon<<','<<r[H::QUEUE]<<','<<control.net<<','
            <<prediction.queue<<','<<prediction.peak<<','<<planned<<",,,conditional_shared_resource\n";
        auto checkTime=(horizon/Ns(m_config.period))*Ns(m_config.period);
        if (checkTime>now) {
            auto check=Replay(r[H::SAMPLE],checkTime,now,r[H::QUEUE],control.net,all);
            if (RecordForecast(item.first,{m_decision,now,int64_t(r[H::SAMPLE]),checkTime,
                                          double(r[H::QUEUE]),control.net,check.queue,planned}))
                m_predictions<<now<<','<<GetId()<<','<<m_decision<<','<<item.first<<",CHECKPOINT,"
                    <<r[H::SAMPLE]<<','<<checkTime<<','<<r[H::QUEUE]<<','<<control.net<<','<<check.queue<<','
                    <<check.peak<<','<<planned<<",,,periodic_resource_sample_grid\n";
        }
    }
    // Sum queues at common rate-change/packet boundaries. Between boundaries
    // each reflected queue is convex, so the sum's maximum is at an endpoint.
    struct PoolQueue { double queue, net, rate=0; };
    struct PoolChange { uint32_t resource; ArrivalChange change; };
    std::map<uint32_t,PoolQueue> pool;
    std::vector<PoolChange> poolChanges;
    int64_t poolSample=-1; double sampledQueues=0, poolTotal=0; bool poolValid=!members.empty();
    for (const auto& item:members) {
        auto resource=m_remoteResources.find(item.first);
        if (resource==m_remoteResources.end()) { poolValid=false; break; }
        const auto& r=resource->second;
        if (poolSample<0) { poolSample=r[H::SAMPLE]; poolTotal=r[H::POOL_QUEUE]; }
        if (int64_t(r[H::SAMPLE])!=poolSample || now-poolSample>Ns(m_config.timeout)) { poolValid=false; break; }
        const auto& all=resourcePlans.at(item.first);
        for (auto id:item.second) if (!m_flows.at(id).history.Covers(poolSample)) poolValid=false;
        double net=double(r[H::SERVICE])-r[H::BACKGROUND];
        pool[item.first]={Replay(poolSample,now,now,r[H::QUEUE],net,all).queue,net};
        sampledQueues+=r[H::QUEUE];
        for (auto change:ArrivalChanges(now,horizon,all)) {
            // Replay up to now already included instantaneous packets at now.
            if (change.time==now) change.bytes=0;
            poolChanges.push_back({item.first,change});
        }
    }
    if (poolValid) {
        std::stable_sort(poolChanges.begin(),poolChanges.end(),[](const PoolChange& a,const PoolChange& b) {
            return a.change.time<b.change.time;
        });
        double outside=std::max(0.0,poolTotal-sampledQueues), peak=outside; int64_t time=now;
        for (const auto& q:pool) peak+=q.second.queue;
        auto observe=[&]() {
            double total=outside; for (const auto& q:pool) total+=q.second.queue;
            peak=std::max(peak,total);
        };
        auto advance=[&](int64_t next) {
            for (auto& q:pool) q.second.queue=std::max(0.0,q.second.queue+
                (q.second.rate-q.second.net*1e-9)*(next-time));
            time=next; observe();
        };
        for (const auto& event:poolChanges) {
            advance(event.change.time);
            pool[event.resource].rate+=event.change.rate;
            pool[event.resource].queue+=event.change.bytes; observe();
        }
        advance(horizon);
        Event("POOL_PLAN_PEAK",0,peak,"interval-average-unmodelled-occupancy-held-constant");
    } else if (!members.empty()) Event("POOL_PLAN_UNKNOWN",0,0,"unaligned_resource_samples_or_history");
    std::vector<QbbNetDevice::InjectionControl> controls;
    for (auto& item:m_flows) {
        auto& f=item.second; if (!f.source) continue;
        auto bucket=dev->ReadInjectionSnapshot(f.id);
        m_log<<std::setprecision(17)<<now<<','<<GetId()<<','<<m_decision<<','<<f.id<<','<<f.generation<<','
            <<f.remoteGeneration<<','<<f.resource<<','<<f.portResource<<','<<f.Queue()<<','<<f.candidate<<','<<f.target<<','
            <<bucket.tokens<<','<<bucket.burst<<','<<f.supply<<','<<f.service<<','<<f.state[H::SAMPLE]<<','
            <<f.state[H::QUEUE]<<','<<df<<','<<db<<','<<f.reason<<'\n';
        controls.push_back({f.id,f.target}); Event("CONTROL",f.id,f.target,f.reason);
    }
    dev->ApplyInjectionControls(controls);
    ReleaseEmptyQueues();
}
double DciGatewayNode::LocalCnpWait(const Flow& f) const {
    auto it=m_registrations.find(f.key);
    return it!=m_registrations.end() && it->second.confirmed
        ? std::max(m_config.localWait,it->second.response) : m_config.localWait;
}
bool DciGatewayNode::SendNearCnp(Flow& f,const char* reason) {
    auto it=m_registrations.find(f.key);
    if (f.completed || it==m_registrations.end() || !it->second.confirmed || !it->second.fastCnp) {
        ++m_fastCnpUnconfirmed;
        return false;
    }
    if (f.lastCnp>=0 && NowNs()-f.lastCnp<Ns(m_config.cnpInterval)) {
        ++m_fastCnpLimited;
        return false;
    }
    H h; h.type=H::FAST_CNP;
    h.v[H::SIP]=f.key.srcIp; h.v[H::DIP]=f.key.dstIp; h.v[H::QPN]=f.key.destQpn;
    h.v[H::CONNECTION]=it->second.connection; h.v[H::SAMPLE]=NowNs();
    if (!Send(h,Ipv4Address(f.key.srcIp))) { ++m_fastCnpSubmitFailed; return false; }
    ++m_fastCnpSubmitted;
    f.responsePending=true; f.responseBaseline=f.supply;
    f.lastCnp=NowNs(); ++f.nearCnps; Event("NEAR_CNP",f.id,f.nearCnps,reason);
    return true;
}
void DciGatewayNode::UpdateBackpressure() {
    auto now=NowNs(); if (now<=m_lastBackpressure) return;
    double dt=(now-m_lastBackpressure)*1e-9, growth=0, queue=0, occupied=0, pendingGrowth=0;
    for (uint32_t i=0;i<GetNDevices();++i) if (auto d=DynamicCast<QbbNetDevice>(GetDevice(i))) occupied+=d->GetQueue()->GetNBytesTotal();
    for (const auto& item:m_flows) if (item.second.source) {
        const auto& f=item.second; queue+=f.Queue();
        double flowGrowth=(double(f.in-f.previousIn)-double(f.dequeued-f.previousTx))/dt;
        growth+=flowGrowth;
        // A slow or idle QP must not set the response horizon of every other QP.
        pendingGrowth+=std::max(0.0,flowGrowth)*LocalCnpWait(f);
    }
    double available=std::max(0.0,double(m_config.bufferBytes)-occupied+queue);
    double pressure=queue+pendingGrowth+m_config.margin;
    double high=std::min(m_config.sourceHigh,.5*available), low=std::min(m_config.sourceLow,.25*available);
    bool emergency=pressure>=available;
    double falling=-double(m_frame)/dt;
    bool trigger=(queue>=high && growth>=falling) || (growth>0 && pressure>=high);
    bool draining=growth<falling && !emergency;
    const bool wasActive=m_backpressureActive;
    if (!emergency && queue<low && pressure<high) m_backpressureActive=false;
    else if (trigger || emergency) m_backpressureActive=true;
    if (wasActive!=m_backpressureActive)
        Event(m_backpressureActive ? "BACKPRESSURE_ENTER" : "BACKPRESSURE_EXIT",0,queue);
    Event("PRESSURE",0,pressure,emergency ? "emergency" : (draining ? "draining" : (queue<low ? "below_low" : "observe")));
    for (auto& item:m_flows) {
        auto& f=item.second; if (!f.source || f.completed || f.in==f.previousIn) continue;
        double contribution=double(f.in-f.previousIn)-double(f.dequeued-f.previousTx);
        if (emergency) {
            SendNearCnp(f,"emergency");
        } else if (m_backpressureActive && !draining &&
                   f.Queue()>m_frame && contribution>double(m_frame)) {
            // Only this QP's accumulating backlog warrants ordinary pressure feedback.
            // A one-frame deadband rejects zero growth and packet-boundary sampling noise.
            // Native and gateway feedback share a response wait only within this QP.
            auto last=std::max(f.lastCnp,f.lastNative);
            if (last<0 || now-last>=Ns(std::max(LocalCnpWait(f),m_config.cnpInterval)))
                SendNearCnp(f,"local_pressure");
        }
    }
    for (auto& item:m_flows) if (item.second.source) {
        auto& f=item.second; f.previousIn=f.in; f.previousTx=f.dequeued;
    }
    m_lastBackpressure=now;
}
void DciGatewayNode::Tick() {
    auto now=NowNs(); ++m_ticks; bool sources=false;
    for (auto& item:m_flows) {
        auto& f=item.second; if (!f.source) continue;
        sources=true;
        const auto delay=m_forwardBase+Port(m_wan)->GetDataRate().CalculateBytesTxTime(m_frame).GetNanoSeconds();
        if (f.history.txSnapshots.back().timeNs<now) {
            if (f.tx!=f.history.txSnapshots.back().cumulativeBytes) f.lastMappedArrival=now+delay;
            f.history.Add(now,f.tx,delay); ++m_historyCount;
        }
        MaintainHistory(f,now);
        m_flowHistoryPeak=std::max(m_flowHistoryPeak,uint64_t(f.history.txSnapshots.size()));
        if (now-f.supplySample>=Ns(m_config.localWindow)) {
            double observed=(f.in-f.supplyIn)/((now-f.supplySample)*1e-9);
            auto reg=m_registrations.find(f.key);
            if (f.responsePending && reg!=m_registrations.end() && f.responseBaseline>0 &&
                f.supplySample>=f.lastCnp+Ns(reg->second.localRtt) && observed<.9*f.responseBaseline) {
                reg->second.response=std::max(reg->second.localRtt,(now-f.lastCnp)*1e-9);
                f.responsePending=false;
                Event("LOCAL_RESPONSE",f.id,reg->second.response,"observed_input_decline_not_causal_proof");
            }
            f.supply=std::min(Port(m_wan)->GetDataRate().GetBitRate()/8.0,observed);
            f.supplyIn=f.in; f.supplySample=now;
        }
    }
    m_historyPeak=std::max(m_historyPeak,m_historyCount);
    for (const auto& sample:m_validationSamples) ValidateForecasts(sample.first,sample.second);
    m_validationSamples.clear();
    ReclaimFlows();
    if (now-m_lastBackpressure>=Ns(m_config.localWindow)) UpdateBackpressure();
    m_controlPending=false;
    if (sources) ActivateSources();
    m_tick=Simulator::Schedule(Seconds(m_config.control),&DciGatewayNode::Tick,this);
}
void DciGatewayNode::WriteFlowMetadata(std::ostream& out,const Flow& f,bool retired) const {
        out<<"{\"id\":"<<f.id<<",\"role\":\""<<(f.source ? 'A' : 'B')
           <<"\",\"generation\":"<<f.generation<<",\"peer_generation\":"<<f.remoteGeneration
           <<",\"src_ip\":"<<f.key.srcIp<<",\"dst_ip\":"<<f.key.dstIp<<",\"dest_qpn\":"<<f.key.destQpn
           <<",\"port\":"<<f.port<<",\"pg\":"<<f.pg<<",\"resource\":"<<f.resource<<",\"port_resource\":"<<f.portResource
           <<",\"in_total\":"<<f.in<<",\"dequeued_total\":"<<f.dequeued<<",\"tx_total\":"<<f.tx
           <<",\"queue_bytes\":"<<f.Queue()<<",\"drops\":"<<f.drops<<",\"tx_failed_bytes\":"<<f.failures
           <<",\"near_cnps\":"<<f.nearCnps<<",\"native_cnps\":"<<f.nativeCnps<<",\"retired\":"<<(retired ? "true" : "false")<<",\"history_records\":"<<(retired ? 0 : f.history.txSnapshots.size())
           <<",\"snapshot_tx_total\":"<<(f.history.txSnapshots.empty() ? 0 : f.history.txSnapshots.back().cumulativeBytes)
           <<",\"unsnapshotted_tx_bytes\":"<<(f.history.txSnapshots.empty() ? 0 : f.tx-f.history.txSnapshots.back().cumulativeBytes)<<'}';
}
void DciGatewayNode::WriteMetadata(std::ostream& out) const {
    out<<"{\"node\":"<<GetId()<<",\"session\":"<<m_config.session<<",\"peer\":"<<m_peer<<",\"wan_port\":"<<m_wan
       <<",\"fast_cnp_enabled\":"<<(m_started ? "true" : "false")
       <<",\"source_ingress_ce_packets\":"<<m_sourceIngressCe<<",\"source_cleared_ce_packets\":"<<m_sourceClearedCe
       <<",\"fast_cnp_candidates\":"<<m_fastCnpCandidates<<",\"fast_cnp_submitted\":"<<m_fastCnpSubmitted
       <<",\"fast_cnp_tx\":"<<m_fastCnpTx<<",\"fast_cnp_tx_failed\":"<<m_fastCnpTxFailed
       <<",\"fast_cnp_replaced_packets\":"<<m_fastCnpReplaced<<",\"fast_cnp_rate_limited\":"<<m_fastCnpLimited
       <<",\"fast_cnp_coalesced_packets\":"<<m_fastCnpCoalesced
       <<",\"fast_cnp_ingress_handled_packets\":"<<m_fastCnpIngressHandled
       <<",\"fast_cnp_ingress_coalesced_packets\":"<<m_fastCnpIngressCoalesced
       <<",\"fast_cnp_unconfirmed\":"<<m_fastCnpUnconfirmed<<",\"fast_cnp_submit_failed\":"<<m_fastCnpSubmitFailed
       <<",\"frame_bytes\":"<<m_frame<<",\"burst_bytes\":"<<4*m_frame<<",\"flows\":[";
    bool comma=false;
    // Archive is reporting only: reclaimed identities do not remain in control memory.
    std::ifstream retired(m_retiredPath); std::string line;
    while (std::getline(retired,line)) { if (comma) out<<','; out<<line; comma=true; }
    for (const auto& item:m_flows) {
        const auto& f=item.second;
        if (comma) out<<','; WriteFlowMetadata(out,f);
        comma=true;
    }
    out<<"],\"live_flows\":"<<m_flows.size()<<",\"retired_flows\":"<<m_retiredCount
       <<",\"control_tx_bytes\":"<<m_controlTx<<",\"control_rx_bytes\":"<<m_controlRx
       <<",\"control_budget_drops\":"<<m_controlDropped<<",\"rejected\":"<<m_rejected<<",\"resource_drops\":"<<m_resourceDrops
       <<",\"history_records\":"<<m_historyCount<<",\"registrations\":"<<m_registrations.size()
       <<",\"performance\":{\"decisions\":"<<m_decision<<",\"ticks\":"<<m_ticks
       <<",\"light_reallocations\":"<<m_reallocations
       <<",\"arrival_triggers\":"<<m_triggers[ARRIVAL]<<",\"empty_triggers\":"<<m_triggers[EMPTY]
       <<",\"flow_feedback_triggers\":"<<m_triggers[FLOW_FEEDBACK]
       <<",\"resource_feedback_triggers\":"<<m_triggers[RESOURCE_FEEDBACK]
       <<",\"pressure_triggers\":"<<m_triggers[PRESSURE]<<",\"pause_triggers\":"<<m_triggers[PAUSE]
       <<",\"coalesced_triggers\":"<<m_coalesced<<",\"immediate_stops\":"<<m_safetyStops
       <<",\"history_copies\":"<<m_historyCopies<<",\"history_copied_records\":"<<m_historyCopiedRecords
       <<",\"future_models\":"<<m_futureModels<<",\"future_packets\":"<<m_futurePackets
       <<",\"forecast_created\":"<<m_forecastCreated<<",\"forecast_validated\":"<<m_forecastValidated
       <<",\"forecast_unavailable\":"<<m_forecastUnavailable<<",\"forecast_expired\":"<<m_forecastExpired
       <<",\"forecast_rejected\":"<<m_forecastRejected<<",\"forecast_peak_per_resource\":"<<m_forecastPeak
       <<",\"history_peak\":"<<m_historyPeak<<",\"history_peak_per_flow\":"<<m_flowHistoryPeak<<'}'
       <<",\"forward_base_ns\":"<<m_forwardBase<<",\"reverse_base_ns\":"<<m_reverseBase<<'}';
}
void DciGatewayNode::DoDispose() {
    for (auto event:{m_tick,m_report,m_probeEvent,m_serviceReport,m_reallocate}) Simulator::Cancel(event);
    if (m_started) for (uint32_t i=0;i<GetNDevices();++i) {
        auto d=DynamicCast<QbbNetDevice>(GetDevice(i)); if (!d) continue;
        if (i==m_wan) {
            d->m_groupClassifier=Callback<uint32_t,Ptr<const Packet>,uint32_t>();
            d->m_injectionEligible=Callback<bool,uint32_t>(); d->m_beforeTransmit=Callback<void,Ptr<Packet>>();
        }
        d->TraceDisconnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,this,i));
        d->TraceDisconnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,this,i));
        d->TraceDisconnectWithoutContext("QbbPauseState",MakeBoundCallback(&DciGatewayNode::PauseChanged,this,i));
        d->m_traceTransmitResult.DisconnectWithoutContext(MakeBoundCallback(&DciGatewayNode::Transmitted,this,i));
    }
    m_flows.clear(); m_data.clear(); m_resources.clear(); m_remoteResources.clear(); m_registrations.clear();
    m_resourceForecasts.clear(); m_resourceControls.clear(); m_retiredHistoryFloor.clear();
    m_validationSamples.clear();
    m_log.close(); m_events.close(); m_packets.close(); m_resourceLog.close(); m_predictions.close(); m_budgetLog.close(); m_retired.close();
    SwitchNode::DoDispose();
}
}
