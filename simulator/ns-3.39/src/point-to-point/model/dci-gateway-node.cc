#include "dci-gateway-node.h"
#include "cn-header.h"
#include "ppp-header.h"
#include "qbb-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(DciGatewayNode);
namespace {
int64_t Ns(double s) { return Seconds(s).GetNanoSeconds(); }
void Add(uint64_t& value,uint64_t n) {
    NS_ABORT_MSG_IF(UINT64_MAX-value<n,"R5 counter overflow"); value+=n;
}
// One fixed-size group state per priority-zero packet; no demand messages.
class R5Header : public Header {
public:
    std::array<uint64_t,17> state{};
    static TypeId GetTypeId() {
        static TypeId tid=TypeId("ns3::R5Header").SetParent<Header>().AddConstructor<R5Header>(); return tid;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize() const override { return 144; }
    void Serialize(Buffer::Iterator i) const override {
        i.WriteHtonU32(5); i.WriteHtonU32(17);
        for (auto v:state) i.WriteHtonU64(v);
    }
    uint32_t Deserialize(Buffer::Iterator i) override {
        if (i.GetRemainingSize()!=GetSerializedSize() || i.ReadNtohU32()!=5 || i.ReadNtohU32()!=17) return 0;
        for (auto& v:state) v=i.ReadNtohU64();
        return GetSerializedSize();
    }
    void Print(std::ostream& out) const override { out << "R5 group=" << state[0]; }
};
}
TypeId DciGatewayNode::GetTypeId() {
    static TypeId tid=TypeId("ns3::DciGatewayNode").SetParent<SwitchNode>().AddConstructor<DciGatewayNode>(); return tid;
}
Ipv4Address DciGatewayNode::ControlAddress(uint32_t node) { return Ipv4Address(0xac1f0000u+node+1); }
Ptr<QbbNetDevice> DciGatewayNode::Port(uint32_t port) const {
    NS_ABORT_MSG_IF(port>=GetNDevices(),"R5 invalid port");
    auto d=DynamicCast<QbbNetDevice>(GetDevice(port)); NS_ABORT_MSG_IF(!d,"R5 requires Qbb port"); return d;
}
void DciGatewayNode::Configure(const Config& c,const std::string& output) {
    NS_ABORT_MSG_IF(m_enabled || !std::isfinite(c.period+c.control+c.tau+c.timeout+c.coldFraction+
        c.conservativeFraction+c.qref+c.margin+c.sourceHigh+c.sourceLow+c.localWait+c.cnpInterval+c.localWindow) ||
        Ns(c.control)<=0 || c.period<c.control || Ns(c.period)%Ns(c.control) ||
        c.localWindow<c.control || Ns(c.localWindow)%Ns(c.control) || c.tau<c.control ||
        c.timeout<=c.period || c.coldFraction<=0 || c.coldFraction>c.conservativeFraction ||
        c.conservativeFraction>1 || c.qref<0 || c.margin<=0 || c.sourceLow<0 ||
        c.sourceHigh<=c.sourceLow || c.localWait<c.localWindow || c.cnpInterval<=0 ||
        c.bufferBytes<=c.margin+c.qref,"invalid R5 configuration");
    m_config=c; m_enabled=true;
    auto prefix=output+".gateway-"+std::to_string(GetId());
    m_log.open(prefix+".groups.csv"); m_events.open(prefix+".events.csv");
    m_packets.open(prefix+".packets.csv"); m_groupPackets.open(prefix+".group-packets.csv");
    NS_ABORT_MSG_IF(!m_log || !m_events || !m_packets || !m_groupPackets,"cannot open R5 logs");
    m_events << "time_ns,node,event_order,event,id,value,reason\n";
    m_packets << "time_ns,node,event_order,event,role,flow,group,bytes,in_total,tx_total,queue_bytes,arrival_ns\n";
    m_groupPackets << "time_ns,node,event_order,event,role,group,bytes,in_total,tx_total,queue_bytes,drop_total,other_total\n";
    m_log << "time_ns,node,event_order,role,group,peer,port,pg,queue_bytes,in_total,tx_total,drop_total,other_total,service_Bps,background_Bps,capacity_Bps,buffer_bytes,target_Bps,candidate_Bps,predicted_queue_bytes,predicted_peak_bytes,prediction_end_ns,fresh,history_covered,reason,state_seq,state_sample_ns,state_ordinal,state_queue_bytes,state_in_total,state_tx_total,pressure_bytes,growth_Bps,protecting,bad_residuals,state_flags,state_emptied,state_pause_ns,state_controlled_total,receiver_interval_ns,receiver_unpaused_service_Bps,receiver_available_fraction\n";
}
void DciGatewayNode::RegisterFlow(const Registration& r) {
    NS_ABORT_MSG_IF(!m_enabled || m_started || !r.id || m_flows.count(r.id) || !r.pg || r.pg>=8 ||
        r.port>=256 || r.remotePort>=256 || r.peer>=2097152 || GetId()>=2097152 ||
        r.propagation.IsNegative() || !r.frameBytes,"invalid R5 association");
    DataKey key{r.sip,r.dip,r.sport,r.dport,r.pg};
    NS_ABORT_MSG_IF(m_data.count(key),"reused R5 connection identity");
    bool source=r.role==Role::Source;
    uint32_t id=(source ? r.peer : GetId())*2048+(source ? r.remotePort : r.port)*8+r.pg;
    auto inserted=m_groups.emplace(id,Group{}); auto& g=inserted.first->second;
    if (inserted.second) {
        g.id=id; g.peer=r.peer; g.port=r.port; g.pg=r.pg; g.role=r.role; g.frame=r.frameBytes;
        g.propagation=r.propagation; g.localResponse=r.localResponse;
        g.capacity=Port(r.port)->GetDataRate().GetBitRate()/8.0;
        if (source) {
            m_sourcePorts[r.port].push_back(id);
            Port(r.port)->ConfigureInjectionGroup(id,r.frameBytes);
        } else {
            NS_ABORT_MSG_IF(m_receiverGroups.count({r.port,r.pg}),"multiple A gateways need explicit resource allocation");
            m_receiverGroups[{r.port,r.pg}]=id;
        }
    } else {
        NS_ABORT_MSG_IF(g.peer!=r.peer || g.port!=r.port || g.role!=r.role ||
            g.propagation!=r.propagation || g.frame!=r.frameBytes,"inconsistent R5 managed path/group");
        g.localResponse=std::max(g.localResponse,r.localResponse);
    }
    auto& f=m_flows[r.id]; f.reg=r; f.group=id; f.queue=r.pg;
    if (source) {
        uint32_t count=0;
        for (const auto& e:m_flows) if (e.first!=r.id && e.second.reg.role==Role::Source && e.second.reg.port==r.port) ++count;
        f.queue=QbbNetDevice::qCnt+count;
        NS_ABORT_MSG_IF(f.queue>=BEgressQueue::fCnt,"R5 exceeds logical queues on source port");
        Port(r.port)->BindInjectionQueue(f.queue,r.pg,id);
    }
    g.flows.push_back(r.id); m_data[key]=r.id;
}
void DciGatewayNode::Start() {
    NS_ABORT_MSG_IF(!m_enabled || m_started || m_groups.empty(),"invalid R5 start");
    for (auto& e:m_groups) {
        auto& g=e.second;
        NS_ABORT_MSG_IF(m_config.timeout<=g.propagation.GetSeconds()+3*m_config.period,
                        "R5 timeout too short for feedback path");
        NS_ABORT_MSG_IF(.8*m_config.bufferBytes/m_groups.size()<=m_config.margin+m_config.qref,
                        "R5 group allowances do not fit shared buffer");
        g.previousPause=Port(g.port)->GetQueuePauseTime(g.pg).GetNanoSeconds();
    }
    for (uint32_t i=0;i<GetNDevices();++i) {
        auto d=DynamicCast<QbbNetDevice>(GetDevice(i)); if (!d) continue;
        NS_ABORT_MSG_IF(d->GetMtu()<164,"R5 state exceeds MTU");
        if (m_sourcePorts.count(i)) d->m_groupClassifier=MakeBoundCallback(&DciGatewayNode::Classify,this,i);
        d->TraceConnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,this,i));
        d->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,this,i));
        d->TraceConnectWithoutContext("QbbPauseState",MakeBoundCallback(&DciGatewayNode::PauseChanged,this,i));
    }
    m_started=true;
    m_tick=Simulator::Schedule(Seconds(m_config.control),&DciGatewayNode::Tick,this);
}
DciGatewayNode::Flow* DciGatewayNode::Match(Ptr<const Packet> p) {
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(h);
    if (h.l3Prot!=0x11) return nullptr;
    auto it=m_data.find({h.sip,h.dip,h.udp.sport,h.udp.dport,h.udp.pg});
    return it==m_data.end() ? nullptr : &m_flows.at(it->second);
}
uint32_t DciGatewayNode::Classify(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t pg) {
    auto f=self->Match(p);
    return f && f->reg.role==Role::Source && f->reg.port==port ? f->queue : pg;
}
void DciGatewayNode::Event(const char* event,uint32_t id,double value,const char* reason) {
    m_events << Simulator::Now().GetNanoSeconds() << ',' << GetId() << ',' << ++m_ordinal << ','
        << event << ',' << id << ',' << std::setprecision(17) << value << ',' << reason << '\n';
}
void DciGatewayNode::PacketEvent(Flow& f,const char* event,uint32_t bytes) {
    auto now=Simulator::Now().GetNanoSeconds();
    int64_t arrival=-1;
    if (f.reg.role==Role::Source && std::string(event)=="TX")
        arrival=now+f.reg.propagation.GetNanoSeconds()+Port(f.reg.port)->GetDataRate().CalculateBytesTxTime(bytes).GetNanoSeconds();
    m_packets << now << ',' << GetId() << ',' << ++m_ordinal << ',' << event << ','
        << (f.reg.role==Role::Source ? "A" : "B") << ',' << f.reg.id << ',' << f.group << ',' << bytes << ','
        << f.in << ',' << f.tx << ',' << f.in-f.tx << ',' << arrival << '\n';
}
void DciGatewayNode::GroupEvent(Group& g,const char* event,uint32_t bytes) {
    g.peak=std::max(g.peak,g.Queue());
    m_groupPackets << Simulator::Now().GetNanoSeconds() << ',' << GetId() << ',' << ++m_ordinal << ',' << event << ','
        << (g.IsSource() ? "A" : "B") << ',' << g.id << ',' << bytes << ',' << g.in << ',' << g.tx << ','
        << g.Queue() << ',' << g.drops << ',' << g.other << '\n';
}
void DciGatewayNode::Admitted(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t,uint32_t pg) {
    auto f=self->Match(p); Group* target=nullptr;
    if (f) {
        NS_ABORT_MSG_IF(f->reg.port!=port,"R5 route changed: invalidate and reconfigure managed path before forwarding");
        Add(f->in,p->GetSize()); self->PacketEvent(*f,"ADMIT",p->GetSize());
        if (f->reg.role==Role::Source) target=&self->m_groups.at(f->group);
    }
    auto it=self->m_receiverGroups.find({port,pg});
    if (it!=self->m_receiverGroups.end()) {
        target=&self->m_groups.at(it->second);
        if (!f || f->group!=target->id) Add(target->other,p->GetSize());
        else Add(target->controlled,p->GetSize());
    }
    if (target) {
        bool activate=target->IsSource() && !target->Queue() && !target->target;
        Add(target->in,p->GetSize()); self->GroupEvent(*target,"ADMIT",p->GetSize());
        // Reallocate on the first arrival, after admission callbacks finish. Do not
        // leave an idle group's new packets blocked until the periodic tick.
        if (activate && self->m_activation.IsExpired())
            self->m_activation=Simulator::ScheduleNow(&DciGatewayNode::ActivateSources,self);
    }
    for (auto& e:self->m_groups) if (!e.second.IsSource() && e.second.port==port && e.second.pg!=pg)
        e.second.shared=true;
}
void DciGatewayNode::Dequeue(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t pg) {
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(h);
    if (h.l3Prot==249) Add(self->m_controlTx,p->GetSize());
    auto f=self->Match(p); Group* target=nullptr;
    if (f && f->reg.port==port) {
        Add(f->tx,p->GetSize()); NS_ABORT_MSG_IF(f->tx>f->in,"R5 flow byte conservation");
        self->PacketEvent(*f,"TX",p->GetSize());
        if (f->reg.role==Role::Source) {
            target=&self->m_groups.at(f->group);
            target->history.Add(Simulator::Now().GetNanoSeconds()+f->reg.propagation.GetNanoSeconds()+
                self->Port(port)->GetDataRate().CalculateBytesTxTime(p->GetSize()).GetNanoSeconds(),p->GetSize());
        }
    }
    auto it=self->m_receiverGroups.find({port,pg});
    if (it!=self->m_receiverGroups.end()) target=&self->m_groups.at(it->second);
    if (target) {
        Add(target->tx,p->GetSize()); NS_ABORT_MSG_IF(target->tx>target->in,"R5 group byte conservation");
        if (!target->Queue()) target->emptied=true;
        self->GroupEvent(*target,"TX",p->GetSize());
    }
    for (auto& e:self->m_groups) if (!e.second.IsSource() && e.second.port==port && e.second.pg!=pg) {
        e.second.shared=true;
    }
}
void DciGatewayNode::NotifyAdmissionDrop(uint32_t port,uint32_t pg,Ptr<const Packet> p) {
    if (!m_enabled) return;
    auto it=m_receiverGroups.find({port,pg});
    if (it!=m_receiverGroups.end()) {
        auto& g=m_groups.at(it->second); Add(g.drops,p->GetSize());
        auto f=Match(p); if (f && f->group==g.id) Add(g.controlled,p->GetSize());
        GroupEvent(g,"DROP",p->GetSize());
    } else if (auto f=Match(p)) {
        auto& g=m_groups.at(f->group); Add(g.drops,p->GetSize()); GroupEvent(g,"DROP",p->GetSize());
    }
}
void DciGatewayNode::PauseChanged(DciGatewayNode* self,uint32_t port,uint32_t pg,bool) {
    auto it=self->m_receiverGroups.find({port,pg});
    if (it==self->m_receiverGroups.end()) return;
    auto& g=self->m_groups.at(it->second);
    // Coalesce transitions, not refresh frames. A short observation interval after
    // resume measures usable service before publishing the recovery estimate.
    if (g.serviceReport.IsExpired())
        g.serviceReport=Simulator::Schedule(Seconds(self->m_config.control),
            &DciGatewayNode::ReportServiceChange,self,g.id);
}
void DciGatewayNode::ReportServiceChange(uint32_t group) {
    SampleReceiver(m_groups.at(group));
}
bool DciGatewayNode::SwitchReceiveFromDevice(Ptr<NetDevice> dev,Ptr<Packet> p,CustomHeader& h) {
    if (m_enabled) {
        if (h.l3Prot==249 && h.dip==ControlAddress(GetId()).Get()) { ReceiveReport(p,h); return true; }
        // Observation only: native CNP and every existing CE bit pass through unchanged.
        bool cnp=h.l3Prot==0xff && h.cnp.ecnBits;
        bool ack=(h.l3Prot==0xfc || h.l3Prot==0xfd) && (h.ack.flags&(1<<qbbHeader::FLAG_CNP));
        if (cnp || ack) for (auto& e:m_flows) {
            auto& f=e.second; const auto& r=f.reg;
            if (h.sip==r.dip && h.dip==r.sip && (cnp ? h.cnp.fid : h.ack.dport)==r.sport &&
                (cnp ? h.cnp.qIndex : h.ack.pg)==r.pg && (cnp || h.ack.sport==r.dport)) {
                ++f.nativeCnps; Event("NATIVE_CNP",r.id,f.nativeCnps);
                if (r.role==Role::Source) f.lastNativeCnp=Simulator::Now().GetNanoSeconds();
            }
        }
    }
    return SwitchNode::SwitchReceiveFromDevice(dev,p,h);
}
void DciGatewayNode::History::Add(int64_t arrival,uint32_t bytes) {
    NS_ABORT_MSG_IF(arrival<floor || (!arrivals.empty() && arrival<arrivals.back().time),"R5 reordered managed path");
    ns3::Add(total,bytes); arrivals.push_back({arrival,bytes,total});
}
void DciGatewayNode::History::Prune(int64_t before) {
    floor=std::max(floor,before);
    while (!arrivals.empty() && arrivals.front().time<=floor) arrivals.pop_front();
}
DciGatewayNode::Prediction DciGatewayNode::History::Replay(int64_t sample,int64_t end,int64_t peakBegin,double q,double net,uint64_t sampledControlled) const {
    NS_ABORT_MSG_IF(!Covers(sample) || sample>end || !std::isfinite(q+net) || q<0,"invalid R5 history replay");
    Prediction result{q,0}; int64_t time=sample;
    auto advance=[&](int64_t next) {
        if (time<peakBegin && next>=peakBegin)
            result.peak=std::max(result.peak,std::max(0.0,q-net*(peakBegin-time)*1e-9));
        q=std::max(0.0,q-net*(next-time)*1e-9); time=next;
        if (time>=peakBegin) result.peak=std::max(result.peak,q);
    };
    if (sample>=peakBegin) result.peak=q;
    for (const auto& a:arrivals) {
        // A sample may precede an arrival at the same ns. Its cumulative controlled
        // arrivals (including admission drops) disambiguate that event boundary.
        if (a.time<sample || (a.time==sample && a.cumulative<=sampledControlled)) continue;
        if (a.time>end) break;
        advance(a.time); q+=a.bytes;
        if (time>=peakBegin) result.peak=std::max(result.peak,q);
    }
    advance(end); result.queue=q; return result;
}
double DciGatewayNode::BufferAllowance(const Group& g) const {
    // Equal policy shares of the configured pool, reduced by all currently occupied queues.
    // Headroom is deliberately not counted as reusable controller space.
    uint64_t occupied=0;
    for (uint32_t i=0;i<GetNDevices();++i) {
        auto d=DynamicCast<QbbNetDevice>(GetDevice(i)); if (d) occupied+=d->GetQueue()->GetNBytesTotal();
    }
    double free=std::max(0.0,double(m_config.bufferBytes)-double(occupied));
    return std::min(.8*m_config.bufferBytes/m_groups.size(),double(g.Queue())+free/m_groups.size());
}
void DciGatewayNode::SampleReceiver(Group& g) {
    auto now=Simulator::Now().GetNanoSeconds();
    Simulator::Cancel(g.serviceReport);
    if (now<=g.lastSample) return; // periodic and transition reports may coincide
    g.serviceInterval=now-g.lastSample;
    double dt=g.serviceInterval*1e-9;
    auto dev=Port(g.port); double capacity=dev->GetDataRate().GetBitRate()/8.0;
    auto pause=dev->GetQueuePauseTime(g.pg).GetNanoSeconds();
    auto pauseDelta=pause-g.previousPause;
    NS_ABORT_MSG_IF(pauseDelta<0 || pauseDelta>g.serviceInterval,"R5 invalid pause accounting");
    int64_t active=g.serviceInterval-pauseDelta;
    g.availability=double(active)/g.serviceInterval;
    bool paused=dev->IsQueuePaused(g.pg), changed=capacity!=g.capacity;
    bool shared=g.shared || dev->GetQueue()->GetNBytesTotal()>g.Queue();
    // Queue emptiness is tracked on every dequeue, not inferred from sample endpoints.
    // Estimate service while unpaused separately from its observed availability.
    if (changed) g.serviceValid=false;
    if (!shared) { g.service=capacity; g.serviceValid=true; }
    else if (!g.emptied && active>0 && !changed) {
        g.service=std::min(capacity,(g.tx-g.previousTx)/(active*1e-9)); g.serviceValid=true;
    }
    // An idle shared queue retains its last valid estimate; unknown service starts conservatively.
    g.service=std::min(g.service,capacity); g.capacity=capacity;
    g.background=(g.other-g.previousOther)/dt; g.buffer=BufferAllowance(g);
    Record r{}; r[GROUP]=g.id; r[SEQ]=++g.seq; r[SAMPLE]=now; r[QUEUE]=g.Queue();
    r[ARRIVED]=g.in; r[DEPARTED]=g.tx; r[DROPPED]=g.drops; r[OTHER]=g.other;
    double unpausedService=g.serviceValid ? g.service : capacity*m_config.coldFraction;
    r[SERVICE]=paused ? 0 : uint64_t(unpausedService*g.availability);
    r[BACKGROUND]=uint64_t(g.background); r[CAPACITY]=uint64_t(capacity); r[BUFFER]=uint64_t(g.buffer);
    r[FLAGS]=VALID|(paused ? PAUSED : 0)|(shared ? SHARED : 0);
    r[CONTROLLED]=g.controlled; r[EMPTY]=g.emptied; r[PAUSE_NS]=pauseDelta; r[ORDINAL]=m_ordinal;
    g.state=r;
    Event("SAMPLE",g.id,r[SERVICE],paused ? "paused" : (!shared ? "exclusive" : (g.emptied ? "retained" : "backlogged")));
    g.previousTx=g.tx; g.previousOther=g.other; g.previousPause=pause; g.lastSample=now;
    g.emptied=g.Queue()==0; g.shared=dev->GetQueue()->GetNBytesTotal()>g.Queue();
    SendReport(g);
}
void DciGatewayNode::SendReport(Group& g) {
    R5Header header; header.state=g.state;
    auto p=Create<Packet>(); p->AddHeader(header);
    Ipv4Header ip; ip.SetSource(ControlAddress(GetId())); ip.SetDestination(ControlAddress(g.peer));
    ip.SetProtocol(249); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); SendNetworkControl(p);
}
bool DciGatewayNode::ApplyRecord(Group& g,const Record& r) {
    auto now=Simulator::Now().GetNanoSeconds();
    if (!g.IsSource() || r[GROUP]!=g.id || !r[SEQ] || r[SAMPLE]>uint64_t(now) ||
        r[FLAGS]&~uint64_t(VALID|PAUSED|SHARED) || !(r[FLAGS]&VALID) ||
        r[EMPTY]>1 || !r[CAPACITY] || r[SERVICE]>r[CAPACITY] ||
        r[ARRIVED]<r[DEPARTED] || r[ARRIVED]-r[DEPARTED]!=r[QUEUE] || r[OTHER]>r[ARRIVED] || r[CONTROLLED]<r[ARRIVED]-r[OTHER] ||
        r[CONTROLLED]-(r[ARRIVED]-r[OTHER])>r[DROPPED] || r[CONTROLLED]>g.tx ||
        ((r[FLAGS]&PAUSED) && r[SERVICE]) ||
        (g.have && (r[SEQ]<=g.state[SEQ] || r[SAMPLE]<=g.state[SAMPLE] ||
          r[ARRIVED]<g.state[ARRIVED] || r[DEPARTED]<g.state[DEPARTED] ||
          r[DROPPED]<g.state[DROPPED] || r[OTHER]<g.state[OTHER] || r[CONTROLLED]<g.state[CONTROLLED]))) return false;
    auto forecast=g.forecasts.find(r[SAMPLE]);
    if (forecast!=g.forecasts.end()) {
        double residual=forecast->second-double(r[QUEUE]);
        Event("RESIDUAL",g.id,residual,"frozen_before_sample");
        g.badResiduals=std::abs(residual)>m_config.margin ? std::min(3u,g.badResiduals+1) : 0;
    }
    g.forecasts.erase(g.forecasts.begin(),g.forecasts.upper_bound(r[SAMPLE]));
    g.state=r; g.have=true; Event("STATE",g.id,r[SEQ]); return true;
}
void DciGatewayNode::ReceiveReport(Ptr<Packet> p,const CustomHeader& h) {
    Add(m_controlRx,p->GetSize()); auto copy=p->Copy(); PppHeader ppp; Ipv4Header ip; R5Header state;
    copy->RemoveHeader(ppp); copy->RemoveHeader(ip);
    if (!copy->RemoveHeader(state)) { ++m_rejected; Event("REJECT",0,m_rejected,"malformed"); return; }
    auto it=m_groups.find(state.state[GROUP]);
    if (it==m_groups.end() || h.sip!=ControlAddress(it->second.peer).Get() || !ApplyRecord(it->second,state.state)) {
        ++m_rejected; Event("REJECT",state.state[GROUP],m_rejected,"identity_or_stale_state");
    } else if (m_started && m_activation.IsExpired()) {
        m_activation=Simulator::ScheduleNow(&DciGatewayNode::ActivateSources,this);
    }
}
void DciGatewayNode::UpdateSource(Group& g) {
    auto now=Simulator::Now().GetNanoSeconds(); auto dev=Port(g.port);
    const double previousCap=g.candidate;
    g.capacity=dev->GetDataRate().GetBitRate()/8.0;
    g.history.Prune(std::max(int64_t(0),now-Ns(m_config.timeout)));
    g.fresh=g.have && now-int64_t(g.state[SAMPLE])<=Ns(m_config.timeout);
    g.covered=g.have && g.history.Covers(g.state[SAMPLE]);
    g.predictionEnd=now+g.propagation.GetNanoSeconds()+dev->GetDataRate().CalculateBytesTxTime(g.frame).GetNanoSeconds();
    g.predicted={}; g.reason="cold"; g.candidate=g.capacity*m_config.coldFraction;
    if (!g.have) return;
    g.service=g.state[SERVICE]; g.background=g.state[BACKGROUND]; g.buffer=g.state[BUFFER];
    if ((g.state[FLAGS]&PAUSED) || g.service==0) { g.candidate=0; g.reason="service_stopped"; return; }
    if (!g.fresh || !g.covered) { g.candidate=0; g.reason=!g.fresh ? "stale" : "history_missing"; return; }
    double net=g.service-g.background;
    g.predicted=g.history.Replay(g.state[SAMPLE],g.predictionEnd,now,g.state[QUEUE],net,g.state[CONTROLLED]);
    g.candidate=std::clamp(std::max(0.0,net)-(g.predicted.queue-m_config.qref)/m_config.tau,0.0,g.capacity);
    g.reason="normal";
    // Freeze a genuinely future prediction for the next periodic B sample. It is never rewritten.
    auto next=(now/Ns(m_config.period)+1)*Ns(m_config.period);
    if (next<=g.predictionEnd && !g.forecasts.count(next))
        g.forecasts[next]=g.history.Replay(g.state[SAMPLE],next,now,g.state[QUEUE],net,g.state[CONTROLLED]).queue;
    while (!g.forecasts.empty() && g.forecasts.begin()->first<now-Ns(m_config.timeout)) g.forecasts.erase(g.forecasts.begin());
    double risk=g.predicted.peak+m_config.margin;
    if (risk>=.9*g.buffer) { g.peakLimited=true; g.peakSeq=g.state[SEQ]; }
    else if (g.peakLimited && g.state[SEQ]>g.peakSeq) g.peakLimited=false;
    if (g.peakLimited || g.badResiduals>=3) {
        g.candidate=std::min(g.candidate,g.capacity*m_config.conservativeFraction);
        // Peak risk forbids increasing the controller cap. Port allocations can
        // be zero merely because the group was idle and must never constrain it.
        if (g.peakLimited) g.candidate=std::min(g.candidate,previousCap);
        g.reason=g.peakLimited ? "peak_conservative" : "residual_conservative";
    }
    if (risk>=g.buffer) { g.candidate=0; g.reason="peak_stop"; }
}
bool DciGatewayNode::NeedCnp(Group& g,Flow& f,bool emergency,bool trigger,bool draining,int64_t now) {
    double wait=std::max(m_config.localWait,g.localResponse.GetSeconds());
    if (f.in==f.previousIn || (!emergency && (!trigger || draining))) return false;
    auto interval=Ns(emergency ? m_config.cnpInterval : std::max(wait,m_config.cnpInterval));
    auto last=emergency ? f.lastCnp : std::max(f.lastCnp,f.lastNativeCnp);
    return last<0 || now-last>=interval;
}
void DciGatewayNode::UpdateBackpressure(Group& g) {
    auto now=Simulator::Now().GetNanoSeconds();
    double dt=m_config.localWindow;
    g.growth=(double(g.in-g.previousLocalIn)-double(g.tx-g.previousLocalTx))/dt;
    double wait=std::max(m_config.localWait,g.localResponse.GetSeconds());
    double available=BufferAllowance(g);
    g.pressure=g.Queue()+std::max(0.0,g.growth)*wait+m_config.margin;
    double high=std::min(m_config.sourceHigh,.5*available), low=std::min(m_config.sourceLow,.25*available);
    bool emergency=g.pressure>=available;
    // A change smaller than one maximum frame per observation window is packet noise.
    double fallingThreshold=-double(g.frame)/dt;
    bool trigger=(g.Queue()>=high && g.growth>=fallingThreshold) || (g.growth>0 && g.pressure>=high);
    bool draining=g.growth<fallingThreshold && !emergency;
    if (emergency || trigger) g.protecting=true;
    else if (g.Queue()<=low && g.growth<=0) g.protecting=false;
    int64_t lastLocalCnp=-1;
    for (auto id:g.flows) lastLocalCnp=std::max(lastLocalCnp,m_flows.at(id).lastCnp);
    bool waiting=lastLocalCnp>=0 && now-lastLocalCnp<Ns(wait);
    Flow* selected=nullptr;
    double largestGrowth=0;
    uint64_t largestInput=0;
    for (auto id:g.flows) {
        auto& f=m_flows.at(id);
        uint64_t input=f.in-f.previousIn;
        double growth=double(input)-double(f.tx-f.previousTx);
        if (emergency) {
            if (NeedCnp(g,f,true,trigger,draining,now)) SendNearCnp(f,"emergency");
        } else if (!waiting && input && (!selected || growth>largestGrowth ||
            (growth==largestGrowth && (input>largestInput ||
             (input==largestInput && f.lastCnp<selected->lastCnp))))) {
            selected=&f; largestGrowth=growth; largestInput=input;
        }
    }
    // One ordinary action per group response round; observe its effect before
    // reducing additional QPs. Emergency pressure still reaches every contributor.
    // Select the main contributor before checking its response wait: a pending
    // native CNP must not redirect ordinary pressure onto a smaller contributor.
    if (selected && NeedCnp(g,*selected,false,trigger,draining,now)) SendNearCnp(*selected,"local_pressure");
    for (auto id:g.flows) {
        auto& f=m_flows.at(id); f.previousIn=f.in; f.previousTx=f.tx;
    }
    g.previousLocalIn=g.in; g.previousLocalTx=g.tx;
}
void DciGatewayNode::SendNearCnp(Flow& f,const char* reason) {
    // The sender QP is (source IP, source UDP port, PG); never substitute the destination QP.
    auto p=Create<Packet>(); p->AddHeader(CnHeader(f.reg.sport,f.reg.pg,3,1,1));
    Ipv4Header ip; ip.SetSource(Ipv4Address(f.reg.dip)); ip.SetDestination(Ipv4Address(f.reg.sip));
    ip.SetProtocol(0xff); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); SendNetworkControl(p);
    f.lastCnp=Simulator::Now().GetNanoSeconds(); ++f.nearCnps; Event("NEAR_CNP",f.reg.id,f.nearCnps,reason);
}
void DciGatewayNode::ActivateSources() {
    // Recheck safety constraints before reopening, including state freshness.
    for (auto& e:m_groups) if (e.second.IsSource()) UpdateSource(e.second);
    CommitControls();
}
void DciGatewayNode::CommitControls() {
    for (const auto& p:m_sourcePorts) {
        // Max-min group caps on one physical WAN port. Empty groups reserve no rate.
        std::vector<uint32_t> pending; double remaining=Port(p.first)->GetDataRate().GetBitRate()/8.0;
        for (auto id:p.second) { auto& g=m_groups.at(id); g.target=0; if (g.Queue()) pending.push_back(id); }
        while (!pending.empty()) {
            double share=remaining/pending.size(); bool removed=false;
            for (auto it=pending.begin();it!=pending.end();) {
                auto& g=m_groups.at(*it);
                if (g.candidate<=share) { g.target=g.candidate; remaining=std::max(0.0,remaining-g.target); it=pending.erase(it); removed=true; }
                else ++it;
            }
            if (!removed) { for (auto id:pending) m_groups.at(id).target=share; break; }
        }
        std::vector<QbbNetDevice::InjectionControl> controls;
        for (auto id:p.second) {
            const auto& g=m_groups.at(id); controls.push_back({id,g.target});
            Event("CONTROL",id,g.target,g.reason);
        }
        Port(p.first)->ApplyInjectionControls(controls);
    }
}
void DciGatewayNode::WriteSamples() {
    auto now=Simulator::Now().GetNanoSeconds();
    for (const auto& e:m_groups) {
        const auto& g=e.second; const auto& r=g.state;
        m_log << std::setprecision(17) << now << ',' << GetId() << ',' << ++m_ordinal << ',' << (g.IsSource() ? "A" : "B")
            << ',' << g.id << ',' << g.peer << ',' << g.port << ',' << g.pg << ',' << g.Queue() << ',' << g.in << ',' << g.tx
            << ',' << g.drops << ',' << g.other << ',' << (g.IsSource() ? g.service : double(r[SERVICE])) << ',' << g.background << ',' << g.capacity << ',' << g.buffer
            << ',' << g.target << ',' << g.candidate << ',' << g.predicted.queue << ',' << g.predicted.peak << ',' << g.predictionEnd
            << ',' << g.fresh << ',' << g.covered << ',' << g.reason << ',' << r[SEQ] << ',' << r[SAMPLE] << ',' << r[ORDINAL]
            << ',' << r[QUEUE] << ',' << r[ARRIVED] << ',' << r[DEPARTED] << ',' << g.pressure << ',' << g.growth << ',' << g.protecting
            << ',' << g.badResiduals << ',' << r[FLAGS] << ',' << r[EMPTY] << ',' << r[PAUSE_NS] << ',' << r[CONTROLLED]
            << ',' << g.serviceInterval << ',' << (g.IsSource() ? 0 : g.service) << ',' << (g.IsSource() ? 0 : g.availability) << '\n';
    }
}
void DciGatewayNode::Tick() {
    auto now=Simulator::Now().GetNanoSeconds();
    for (auto& e:m_groups) {
        auto& g=e.second;
        if (!g.IsSource()) {
            NS_ABORT_MSG_IF(g.Queue()!=Port(g.port)->GetQueue()->GetNBytes(g.pg),"R5 entire PG accounting mismatch");
            if (now%Ns(m_config.period)==0) SampleReceiver(g);
        } else {
            UpdateSource(g);
            if (now%Ns(m_config.localWindow)==0) UpdateBackpressure(g);
        }
    }
    // Controls become effective at this event; already dequeued frames are in history.
    CommitControls(); WriteSamples();
    m_tick=Simulator::Schedule(Seconds(m_config.control),&DciGatewayNode::Tick,this);
}
void DciGatewayNode::WriteMetadata(std::ostream& out) const {
    out << "{\"node\":" << GetId() << ",\"groups\":["; bool comma=false;
    for (const auto& e:m_groups) {
        const auto& g=e.second;
        out << (comma ? "," : "") << "{\"id\":" << g.id << ",\"role\":\"" << (g.IsSource() ? "A" : "B")
            << "\",\"peer\":" << g.peer << ",\"port\":" << g.port << ",\"pg\":" << g.pg << ",\"frame_bytes\":" << g.frame
            << ",\"policy_buffer_share_bytes\":" << .8*m_config.bufferBytes/m_groups.size()
            << ",\"propagation_ns\":" << g.propagation.GetNanoSeconds() << ",\"local_response_ns\":" << g.localResponse.GetNanoSeconds()
            << ",\"in_total\":" << g.in << ",\"tx_total\":" << g.tx << ",\"drop_bytes\":" << g.drops << ",\"peak_bytes\":" << g.peak << '}'; comma=true;
    }
    out << "],\"flows\":["; comma=false;
    for (const auto& e:m_flows) {
        const auto& f=e.second; const auto& r=f.reg;
        out << (comma ? "," : "") << "{\"id\":" << r.id << ",\"role\":\"" << (r.role==Role::Source ? "A" : "B")
            << "\",\"group\":" << f.group << ",\"peer\":" << r.peer << ",\"port\":" << r.port << ",\"pg\":" << r.pg
            << ",\"in_total\":" << f.in << ",\"tx_total\":" << f.tx << ",\"near_cnps\":" << f.nearCnps << ",\"native_cnps\":" << f.nativeCnps << '}'; comma=true;
    }
    out << "],\"control_tx_bytes\":" << m_controlTx << ",\"control_rx_bytes\":" << m_controlRx << ",\"rejected\":" << m_rejected << '}';
}
void DciGatewayNode::DoDispose() {
    Simulator::Cancel(m_tick);
    Simulator::Cancel(m_activation);
    for (auto& e:m_groups) Simulator::Cancel(e.second.serviceReport);
    if (m_started) for (uint32_t i=0;i<GetNDevices();++i) {
        auto d=DynamicCast<QbbNetDevice>(GetDevice(i)); if (!d) continue;
        if (m_sourcePorts.count(i)) d->m_groupClassifier=Callback<uint32_t,Ptr<const Packet>,uint32_t>();
        d->TraceDisconnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,this,i));
        d->TraceDisconnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,this,i));
        d->TraceDisconnectWithoutContext("QbbPauseState",MakeBoundCallback(&DciGatewayNode::PauseChanged,this,i));
    }
    m_groups.clear(); m_flows.clear(); m_data.clear(); m_receiverGroups.clear(); m_sourcePorts.clear();
    m_log.close(); m_events.close(); m_packets.close(); m_groupPackets.close(); SwitchNode::DoDispose();
}
}
