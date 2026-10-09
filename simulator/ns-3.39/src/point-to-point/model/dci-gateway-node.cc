#include "dci-gateway-node.h"
#include "cn-header.h"
#include "ppp-header.h"
#include "qbb-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <set>

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(DciGatewayNode);
namespace {
void Add(uint64_t& value,uint64_t amount=1) {
    NS_ABORT_MSG_IF(UINT64_MAX-value<amount,"R4 counter overflow"); value+=amount;
}
int64_t Ns(double seconds) { return Seconds(seconds).GetNanoSeconds(); }
class R4Header : public Header {
public:
    uint32_t kind=0;
    std::vector<std::array<uint64_t,19>> records;
    static TypeId GetTypeId() {
        static TypeId tid=TypeId("ns3::R4Header").SetParent<Header>().AddConstructor<R4Header>(); return tid;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t RecordBytes() const { return kind==1 ? 152 : 64; }
    uint32_t GetSerializedSize() const override { return 16+records.size()*RecordBytes(); }
    void Serialize(Buffer::Iterator i) const override {
        i.WriteHtonU32(4); i.WriteHtonU32(kind); i.WriteHtonU32(records.size()); i.WriteHtonU32(RecordBytes());
        for (const auto& r:records) for (size_t j=0;j<RecordBytes()/8;++j) i.WriteHtonU64(r[j]);
    }
    uint32_t Deserialize(Buffer::Iterator i) override {
        if (i.GetRemainingSize()<16) return 0;
        auto version=i.ReadNtohU32(); kind=i.ReadNtohU32();
        auto count=i.ReadNtohU32(),bytes=i.ReadNtohU32();
        if (version!=4 || (kind!=1 && kind!=2) || bytes!=RecordBytes() || !count ||
            count>i.GetRemainingSize()/bytes || i.GetRemainingSize()%bytes ||
            count!=i.GetRemainingSize()/bytes) return 0;
        records.resize(count);
        for (auto& r:records) for (size_t j=0;j<bytes/8;++j) r[j]=i.ReadNtohU64();
        return GetSerializedSize();
    }
    void Print(std::ostream& o) const override { o << "R4 kind=" << kind << " records=" << records.size(); }
};
}
TypeId DciGatewayNode::GetTypeId() {
    static TypeId tid=TypeId("ns3::DciGatewayNode").SetParent<SwitchNode>().AddConstructor<DciGatewayNode>(); return tid;
}
Ipv4Address DciGatewayNode::ControlAddress(uint32_t node) { return Ipv4Address(0xac1f0000u+node+1); }
Ptr<QbbNetDevice> DciGatewayNode::Port(uint32_t port) const {
    NS_ABORT_MSG_IF(port>=GetNDevices(),"R4 invalid port");
    auto dev=DynamicCast<QbbNetDevice>(GetDevice(port)); NS_ABORT_MSG_IF(!dev,"R4 requires Qbb port"); return dev;
}
void DciGatewayNode::Configure(const Config& c,const std::string& output) {
    NS_ABORT_MSG_IF(m_enabled,"R4 configured twice");
    NS_ABORT_MSG_IF(c.ecnMode!="source-boundary" && c.ecnMode!="path","invalid R4 ECN mode");
    NS_ABORT_MSG_IF(c.ecnMode=="source-boundary" && (!c.nearCnp || !c.shaperEcn),
        "R4 source-boundary requires near CNP and the default legacy shaper flag; use path mode for legacy ablations");
    NS_ABORT_MSG_IF(c.startupMode!="optimistic" && c.startupMode!="probe","invalid R4 startup mode");
    NS_ABORT_MSG_IF(!std::isfinite(c.period+c.control+c.bin+c.reaction+c.gamma+c.recovery+c.probeRate+
        c.increase+c.utilization+c.qref+c.tau+c.timeout+c.fallback+c.sourceHigh+c.sourceLow+c.cnpInterval+
        c.reportMin+c.serviceWindow+c.serviceMinBusy+c.probePortFraction) ||
        Ns(c.control)<=0 || Ns(c.bin)<=0 || c.reaction<c.control || c.recovery<c.control ||
        c.gamma<=0 || c.gamma>=1 || c.probeRate<=0 || c.increase<=0 || c.utilization<=0 || c.utilization>1 ||
        c.qref<0 || c.tau<=0 || c.timeout<=c.period || c.fallback<=0 || c.fallback>1 ||
        c.sourceLow<=0 || c.sourceHigh<=c.sourceLow || c.cnpInterval<=0 ||
        c.reportMin<c.control || c.period<c.reportMin || c.serviceWindow<c.control ||
        c.serviceMinBusy<c.control || c.serviceMinBusy>c.serviceWindow ||
        Ns(c.serviceWindow)%Ns(c.control) || Ns(c.serviceMinBusy)%Ns(c.control) ||
        c.probePortFraction<=0 || c.probePortFraction>1 || !c.probeBytes || !c.probePortBurst ||
        c.controlIpMtu<20+16+152 || (c.serviceMode!="budget" && c.serviceMode!="busy-observed"),
        "invalid R4 configuration");
    m_config=c; m_enabled=true;
    auto prefix=output+".gateway-"+std::to_string(GetId());
    m_log.open(prefix+".flows.csv"); m_portLog.open(prefix+".ports.csv");
    m_events.open(prefix+".events.csv"); m_packets.open(prefix+".packets.csv");
    NS_ABORT_MSG_IF(!m_log || !m_portLog || !m_events || !m_packets,"cannot open R4 logs");
    m_packets << "time_ns,node,event_order,event,role,flow,generation,bytes,in_total,tx_total,queue_bytes\n";
    m_events << "time_ns,node,event_order,event,flow,value,reason\n";
    m_log << "time_ns,node,event_order,role,flow,generation,peer,port,pg,life,queue_bytes,peak_queue_bytes,in_total,tx_total,reaction_bps,budget_bps,service_bps,queue_target_bytes,predicted_queue_bytes,queue_used_bytes,candidate_bps,target_bps,fresh,history_covered,mode,reason,state_seq,state_sample_ns,state_ordinal,state_queue_bytes,state_in_total,state_tx_total,state_age_ns,prediction_end_ns,paused,service_source,window_begin_ns,window_end_ns,busy_ns,window_tx_bytes,window_pause_ns,last_budget_change_ns,probe_epoch,probe_remaining,probe_sent,cnp_count,decrease_count,source_high_bytes,source_low_bytes,near_marking,control_tx_bytes,control_rx_bytes,rejected,admission_drops,upstream_ce_packets,upstream_ce_pending,near_cnp_count,active_state_received\n";
    m_portLog << "time_ns,node,port,role,physical_Bps,available_Bps,target_Bps,controlled_tx_bytes,other_tx_bytes,queue_target_bytes,probe_tokens,probe_sent_bytes,buffer_protection\n";
}
void DciGatewayNode::RegisterFlow(const Registration& r) {
    NS_ABORT_MSG_IF(!m_enabled || m_started || !r.id || !r.generation || m_flows.count(r.id) ||
        !r.pg || r.pg>=8 || r.forwardDelay.GetNanoSeconds()<=0 || r.feedbackRtt.GetNanoSeconds()<=0 ||
        r.start.GetNanoSeconds()<0 || !r.frameBytes || r.frameBytes>m_config.probeBytes ||
        r.frameBytes>m_config.probePortBurst || !std::isfinite(r.peerPortBytesPerSec) || r.peerPortBytesPerSec<=0,
        "invalid R4 registration");
    DataKey key{r.sip,r.dip,r.sport,r.dport,r.pg}; FeedbackKey feedback{r.dip,r.sip,r.sport,r.pg};
    NS_ABORT_MSG_IF(m_data.count(key) || m_feedback.count(feedback),"ambiguous/reused R4 wire identity");
    auto dev=Port(r.port); auto& port=m_ports[r.port];
    NS_ABORT_MSG_IF(port.flows.size()>=BEgressQueue::fCnt-QbbNetDevice::qCnt,"R4 exceeds 120 flows per port");
    NS_ABORT_MSG_IF(!port.flows.empty() && m_flows.at(port.flows.front()).reg.role!=r.role,
                    "R4 mixed Source/Receiver roles on one port unsupported");
    auto& f=m_flows[r.id]; f.reg=r; f.queue=QbbNetDevice::qCnt+port.flows.size();
    f.rate=std::min(m_config.probeRate,dev->GetDataRate().GetBitRate()/8.0);
    port.flows.push_back(r.id); std::sort(port.flows.begin(),port.flows.end());
    port.queueToFlow[f.queue]=r.id; port.physical=dev->GetDataRate().GetBitRate()/8.0;
    m_data[key]=r.id; m_feedback[feedback]=r.id;
    dev->ConfigureGroupShaper(f.queue,r.pg,0,r.frameBytes);
}
void DciGatewayNode::Start() {
    if (!m_enabled) return;
    NS_ABORT_MSG_IF(m_started,"R4 started twice");
    double maxDelay=0,targets=0,high=0;
    for (const auto& e:m_flows) {
        const auto& r=e.second.reg; maxDelay=std::max(maxDelay,r.forwardDelay.GetSeconds());
        NS_ABORT_MSG_IF(m_config.timeout<=r.forwardDelay.GetSeconds()+3*m_config.period,"R4 timeout too short for WAN feedback");
    }
    for (const auto& e:m_ports) {
        bool source=m_flows.at(e.second.flows.front()).IsSource();
        targets+=source ? 0 : m_config.qref; high+=source ? m_config.sourceHigh : 0;
    }
    NS_ABORT_MSG_IF(targets+high>=.8*m_config.bufferBytes,"R4 port thresholds exceed controlled buffer safety budget");
    // Every outgoing interface may carry a routed control packet.
    for (uint32_t i=0;i<GetNDevices();++i) {
        auto d=DynamicCast<QbbNetDevice>(GetDevice(i));
        NS_ABORT_MSG_IF(d && d->GetMtu()<m_config.controlIpMtu,"R4 control MTU exceeds link MTU");
    }
    for (auto& e:m_flows) {
        auto& f=e.second;
        if (f.IsSource()) f.source.history.Init(m_config.bin,m_config.timeout+maxDelay+2*m_config.bin);
        else f.receiver.window.Init(Ns(m_config.serviceWindow)/Ns(m_config.control),
            Simulator::Now().GetNanoSeconds(),Port(f.reg.port)->GetQueuePauseTime(f.reg.pg).GetNanoSeconds());
    }
    for (const auto& e:m_ports) {
        auto d=Port(e.first);
        d->ConfigureProbeBucket(e.second.physical*m_config.probePortFraction,m_config.probePortBurst);
        d->m_groupClassifier=MakeBoundCallback(&DciGatewayNode::Classify,this,e.first);
        d->TraceConnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,this,e.first));

    }
    for (uint32_t i=0;i<GetNDevices();++i) {
        auto d=DynamicCast<QbbNetDevice>(GetDevice(i));
        if (d) d->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,this,i));
    }
    m_started=true;
    m_tick=Simulator::Schedule(Seconds(m_config.control),&DciGatewayNode::Tick,this);
}
void DciGatewayNode::CompleteFlow(uint32_t id) {
    auto it=m_flows.find(id);
    if (it!=m_flows.end() && it->second.IsSource() && !it->second.closed) {
        it->second.closed=true; it->second.dirty=true; Event("COMPLETE",id,0);
    }
}
DciGatewayNode::Flow* DciGatewayNode::Match(Ptr<const Packet> p) {
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(h);
    if (h.l3Prot!=0x11) return nullptr;
    auto it=m_data.find({h.sip,h.dip,h.udp.sport,h.udp.dport,h.udp.pg});
    return it==m_data.end() ? nullptr : &m_flows.at(it->second);
}
bool DciGatewayNode::ShouldMarkEcn(uint32_t port,Ptr<const Packet> p) {
    if (!m_enabled) return true;
    auto f=Match(p);
    if (!f || f->reg.port!=port) return true;
    // Shaping backlog at either gateway must not feed back into B's service budget.
    // Existing CE is consumed only at A; B preserves it for downstream feedback.
    return m_config.ecnMode=="path" && m_config.shaperEcn;
}
uint32_t DciGatewayNode::Classify(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t pg) {
    auto f=self->Match(p); return f && f->reg.port==port ? f->queue : pg;
}
void DciGatewayNode::PacketEvent(Flow& f,const char* event,uint32_t bytes) {
    Add(m_ordinal);
    m_packets << Simulator::Now().GetNanoSeconds() << ',' << GetId() << ',' << m_ordinal << ',' << event << ','
        << (f.IsSource() ? "A" : "B") << ',' << f.reg.id << ',' << f.reg.generation << ',' << bytes << ','
        << f.in << ',' << f.tx << ',' << f.Queue() << '\n';
}
void DciGatewayNode::Admitted(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t queue,uint32_t) {
    auto it=self->m_ports.at(port).queueToFlow.find(queue);
    if (it==self->m_ports.at(port).queueToFlow.end()) return;
    auto& f=self->m_flows.at(it->second); auto now=Simulator::Now().GetNanoSeconds();
    if (!f.IsSource()) f.receiver.window.AccountUntil(now);
    Add(f.in,p->GetSize()); f.lastArrival=now; f.peak=std::max(f.peak,f.Queue()); f.dirty=true;
    if (!f.IsSource()) f.receiver.window.backlogged=true;
    self->PacketEvent(f,"ADMIT",p->GetSize());
}
void DciGatewayNode::Dequeue(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t) {
    CustomHeader header(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
    p->PeekHeader(header);
    if (header.l3Prot==249) Add(self->m_controlTx,p->GetSize());
    if (!self->m_ports.count(port)) return;
    auto& portState=self->m_ports.at(port);
    auto it=portState.queueToFlow.find(self->Port(port)->GetQueue()->GetLastLogicalQueue());
    if (it==portState.queueToFlow.end()) { Add(portState.otherTx,p->GetSize()); return; }
    auto& f=self->m_flows.at(it->second); auto now=Simulator::Now().GetNanoSeconds();
    if (!f.IsSource()) f.receiver.window.AccountUntil(now);
    Add(f.tx,p->GetSize()); NS_ABORT_MSG_IF(f.tx>f.in,"R4 byte conservation failed");
    if (f.IsSource()) f.source.history.Add(now,p->GetSize());
    else { Add(f.receiver.window.bytes,p->GetSize()); f.receiver.window.backlogged=f.Queue()>0; }
    self->PacketEvent(f,"TX",p->GetSize());
    if (f.IsSource() && (f.source.mode==TxMode::StartupProbe || f.source.mode==TxMode::RecoveryProbe) &&
        self->Port(port)->GetProbeCounters(f.queue).remaining<f.reg.frameBytes)
        self->Event("PROBE_EXHAUST",f.reg.id,self->Port(port)->GetProbeCounters(f.queue).remaining);
}
void DciGatewayNode::Event(const char* name,uint32_t id,double value,const char* reason) {
    Add(m_ordinal);
    m_events << Simulator::Now().GetNanoSeconds() << ',' << GetId() << ',' << m_ordinal << ','
        << name << ',' << id << ',' << std::setprecision(17) << value << ',' << reason << '\n';
}
bool DciGatewayNode::SwitchReceiveFromDevice(Ptr<NetDevice> dev,Ptr<Packet> p,CustomHeader& h) {
    if (m_enabled) {
        if (h.l3Prot==249 && h.dip==ControlAddress(GetId()).Get()) { ReceiveRecords(p,h); return true; }
        ObserveFeedback(h);
        ConsumeSourceCe(p,h);
    }
    return SwitchNode::SwitchReceiveFromDevice(dev,p,h);
}
void DciGatewayNode::ConsumeSourceCe(Ptr<Packet> p,CustomHeader& h) {
    if (!m_enabled || m_config.ecnMode!="source-boundary" || h.l3Prot!=0x11 ||
        h.GetIpv4EcnBits()!=CustomHeader::ECN_CE) return;
    auto f=Match(p);
    if (!f || !f->IsSource() || LookupOutputPort(p,h)!=int(f->reg.port)) return;
    // Consume the source-domain signal before clearing it. Pending feedback survives
    // an empty queue and rate limiting; only an actual near CNP consumes it.
    f->upstreamCePending=true; Add(f->upstreamCe);
    PppHeader ppp; Ipv4Header ip;
    p->RemoveHeader(ppp); p->RemoveHeader(ip);
    if (Node::ChecksumEnabled()) ip.EnableChecksum();
    ip.SetEcn(Ipv4Header::ECN_ECT0); h.m_tos=ip.GetTos();
    p->AddHeader(ip); p->AddHeader(ppp);
    Event("SOURCE_CE",f->reg.id,f->upstreamCe,"converted_to_near_pending");
}
void DciGatewayNode::ObserveFeedback(CustomHeader& h) {
    bool cnp=h.l3Prot==0xff && h.cnp.ecnBits;
    bool ack=(h.l3Prot==0xfc || h.l3Prot==0xfd) && (h.ack.flags&(1<<qbbHeader::FLAG_CNP));
    if (!cnp && !ack) return;
    auto it=m_feedback.find({h.sip,h.dip,cnp ? h.cnp.fid : h.ack.dport,cnp ? h.cnp.qIndex : h.ack.pg});
    if (it==m_feedback.end() || (ack && m_flows.at(it->second).reg.dport!=h.ack.sport)) {
        Add(m_unmatched); Event("UNMATCHED_CNP",0,m_unmatched); return;
    }
    auto& f=m_flows.at(it->second); f.lastCnp=Simulator::Now().GetNanoSeconds(); Add(f.cnps);
    if (!f.IsSource()) f.event=true;
    Event("NATIVE_CNP",f.reg.id,f.cnps);
}
std::vector<double> DciGatewayNode::Allocate(const std::vector<double>& caps,double capacity) {
    NS_ABORT_MSG_IF(!std::isfinite(capacity) || capacity<0,"invalid R4 capacity");
    for (auto cap:caps) NS_ABORT_MSG_IF(!std::isfinite(cap) || cap<0,"invalid R4 rate cap");
    std::vector<double> sorted=caps,result; std::sort(sorted.begin(),sorted.end());
    double remaining=capacity,level=0;
    for (size_t i=0;i<sorted.size();++i) {
        level=remaining/(sorted.size()-i);
        if (sorted[i]>=level) break;
        remaining=std::max(0.0,remaining-sorted[i]);
    }
    // Bound the accumulated floating-point sum as well as each mathematical share.
    double total=0;
    for (auto cap:caps) {
        double share=std::min({cap,level,std::max(0.0,capacity-total)});
        result.push_back(share); total+=share;
    }
    return result;
}
void DciGatewayNode::History::Init(double w,double retention) {
    width=Ns(w); NS_ABORT_MSG_IF(width<=0 || !std::isfinite(retention) || retention<0,"invalid history interval");
    bins.assign(size_t((Ns(retention)+width-1)/width)+1,0); latest=0;
}
void DciGatewayNode::History::Advance(int64_t now) {
    NS_ABORT_MSG_IF(now<0 || bins.empty(),"invalid history time/storage");
    int64_t next=now/width; NS_ABORT_MSG_IF(next<latest,"history time reversed");
    if (next-latest>=int64_t(bins.size())) std::fill(bins.begin(),bins.end(),0);
    else for (int64_t b=latest+1;b<=next;++b) bins[b%bins.size()]=0;
    latest=next;
}
void DciGatewayNode::History::Add(int64_t now,uint32_t bytes) { Advance(now); ns3::Add(bins[latest%bins.size()],bytes); }
bool DciGatewayNode::History::Covers(int64_t begin,int64_t end) const {
    return !bins.empty() && end>=0 && end>=begin && end/width<=latest &&
        std::max(int64_t(0),begin)/width>=std::max(int64_t(0),latest-int64_t(bins.size())+1);
}
double DciGatewayNode::History::Predict(int64_t sample,int64_t delay,int64_t now,double q,double service) const {
    NS_ABORT_MSG_IF(sample<0 || delay<0 || now<sample || !std::isfinite(q+service) || q<0 || service<0 ||
        !Covers(sample-delay,now),"invalid/missing R4 history");
    for (int64_t t=sample-delay;t<now;) {
        int64_t next=std::min(now,t<0 ? int64_t(0) : (t/width+1)*width);
        double bytes=0;
        if (t>=0) {
            auto base=t/width*width,end=std::min(base+width,now);
            if (end>base) bytes=double(bins[(t/width)%bins.size()])*(next-t)/(end-base);
        }
        q=std::max(0.0,q+bytes-service*(next-t)*1e-9); t=next;
    }
    NS_ABORT_MSG_IF(!std::isfinite(q),"nonfinite history prediction"); return q;
}
void DciGatewayNode::ServiceWindow::Init(size_t length,int64_t now,int64_t pause) {
    NS_ABORT_MSG_IF(!length,"empty service window");
    slices.assign(length,{}); last=begin=now; previousPause=pause;
}
void DciGatewayNode::ServiceWindow::AccountUntil(int64_t now) {
    NS_ABORT_MSG_IF(now<last,"service time reversed");
    if (backlogged) busy+=now-last;
    last=now;
}
void DciGatewayNode::ServiceWindow::Close(int64_t now,int64_t pause) {
    AccountUntil(now);
    NS_ABORT_MSG_IF(slices.empty() || pause<previousPause || pause-previousPause>now-begin,
                    "invalid service/pause observation");
    slices[next]={begin,now,busy,pause-previousPause,bytes};
    next=(next+1)%slices.size(); count=std::min(count+1,slices.size());
    observation={slices[(next+slices.size()-count)%slices.size()].begin,now,0,0,0};
    for (size_t i=0;i<count;++i) {
        const auto& s=slices[(next+slices.size()-1-i)%slices.size()];
        observation.busy+=s.busy; observation.pause+=s.pause; Add(observation.bytes,s.bytes);
    }
    begin=now; busy=0; bytes=0; previousPause=pause;
}
void DciGatewayNode::SamplePorts() {
    auto now=Simulator::Now().GetNanoSeconds();
    for (auto& e:m_ports) {
        auto& p=e.second; double other=0;
        if (p.lastSample>=0) {
            NS_ABORT_MSG_IF(now<=p.lastSample,"nonpositive capacity sample interval");
            other=(p.otherTx-p.previousOtherTx)/((now-p.lastSample)*1e-9);
        } else Event("CAPACITY_INITIAL",e.first,0,"no_previous_interval");
        p.available=std::max(0.0,m_config.utilization*p.physical-other);
        p.previousOtherTx=p.otherTx; p.lastSample=now;
    }
}
void DciGatewayNode::UpdateDemand() {
    auto now=Simulator::Now().GetNanoSeconds(); uint64_t a=0,b=0;
    for (auto& e:m_flows) {
        auto& f=e.second; bool was=f.active,paused=f.paused;
        f.paused=Port(f.reg.port)->IsQueuePaused(f.reg.pg);
        if (f.IsSource()) f.active=f.Queue()>0 || (!f.closed && now>=f.reg.start.GetNanoSeconds());
        else {
            auto& d=f.receiver.demand;
            f.active=f.Queue()>0 || (!f.closed && d[2] && now-int64_t(d[3])<=Ns(m_config.timeout) && (d[6]&HAS_DEMAND)) ||
                (!f.closed && f.lastArrival>=0 && now-f.lastArrival<=2*f.reg.forwardDelay.GetNanoSeconds()+f.reg.feedbackRtt.GetNanoSeconds());
            if (f.active && !was) {
                bool optimistic=m_config.startupMode=="optimistic" && !f.receiver.started;
                f.receiver.started=true; f.probing=!optimistic;
                f.rate=optimistic ? m_ports.at(f.reg.port).physical :
                    std::min(m_config.probeRate,m_ports.at(f.reg.port).physical);
                f.recoverySince=now; f.previousTx=f.tx;
            }
        }
        f.life=f.closed ? (f.Queue() ? Life::Draining : Life::Closed) :
            now<f.reg.start.GetNanoSeconds() ? Life::NotStarted : f.active ? Life::Active : Life::Idle;
        if (paused!=f.paused || was!=f.active) f.dirty=true;
        Add(f.IsSource() ? a : b,f.Queue());
    }
    auto protection=[this](uint64_t bytes,bool& flag,const char* role) {
        bool before=flag;
        if (!flag && bytes>.8*m_config.bufferBytes) flag=true;
        else if (flag && bytes<.5*m_config.bufferBytes) flag=false;
        if (flag!=before) Event("BUFFER_PROTECTION",GetId(),flag,role);
    };
    protection(a,m_sourceOverload,"A_aggregate"); protection(b,m_receiverOverload,"B_aggregate");
    for (auto& e:m_ports) {
        bool source=m_flows.at(e.second.flows.front()).IsSource();
        if (source ? m_sourceOverload : m_receiverOverload) e.second.available*=m_config.fallback;
    }
}
void DciGatewayNode::UpdateReceiver() {
    auto now=Simulator::Now().GetNanoSeconds();
    for (auto& e:m_flows) {
        auto& f=e.second; if (f.IsSource()) continue;
        if (now-f.reactionStart>=Ns(m_config.reaction)) {
            f.alpha=(1-m_config.gamma)*f.alpha+m_config.gamma*(f.event ? 1 : 0);
            if (f.event) {
                f.rate=std::min(m_ports.at(f.reg.port).physical,std::max(m_config.probeRate,f.rate*(1-f.alpha/2)));
                Add(f.decreases); f.recoverySince=now; f.dirty=true; Event("B_DECREASE",f.reg.id,f.rate*8);
            }
            f.event=false; f.reactionStart=now;
        }
        auto wait=std::max(Ns(m_config.recovery),f.reg.feedbackRtt.GetNanoSeconds());
        if (f.paused || (f.lastCnp>=0 && now-f.lastCnp<wait)) {
            f.recoverySince=now; f.previousTx=f.tx;
        } else if (now-f.recoverySince>=wait) {
            if (f.tx>f.previousTx && f.lastArrival>=0 && now-f.lastArrival<=wait) {
                f.rate=std::min(m_ports.at(f.reg.port).physical,f.rate+m_config.increase);
                f.probing=false; Event("B_RECOVER",f.reg.id,f.rate*8);
            }
            f.recoverySince=now; f.previousTx=f.tx;
        }
    }
    for (auto& e:m_ports) {
        auto& p=e.second; if (m_flows.at(p.flows.front()).IsSource()) continue;
        std::vector<double> caps;
        for (auto id:p.flows) {
            auto& f=m_flows.at(id);
            double cap=f.rate;
            if (f.probing) cap=std::min(cap,m_config.probeBytes/std::max(f.reg.feedbackRtt.GetSeconds(),m_config.recovery));
            caps.push_back(f.active && !f.paused ? cap : 0);
        }
        auto rates=Allocate(caps,p.available); double sum=std::accumulate(rates.begin(),rates.end(),0.0);
        for (size_t i=0;i<rates.size();++i) {
            auto& f=m_flows.at(p.flows[i]); auto& b=f.receiver;
            auto target=sum>0 ? uint64_t(std::floor(m_config.qref*rates[i]/sum)) : 0;
            if (f.budget!=rates[i] || b.queueTarget!=target) f.dirty=true;
            if (f.budget!=rates[i]) b.lastBudgetChange=now;
            f.target=f.budget=rates[i]; b.queueTarget=target;
            const auto& w=b.window.observation;
            if (f.paused) { b.service=0; b.serviceSource=ServiceSource::Paused; }
            else if (m_config.serviceMode=="budget") { b.service=f.budget; b.serviceSource=ServiceSource::BudgetOnly; }
            else if (w.busy<Ns(m_config.serviceMinBusy)) { b.service=f.budget; b.serviceSource=ServiceSource::BudgetFallback; }
            else { b.service=std::min(f.budget,w.bytes/(w.busy*1e-9)); b.serviceSource=ServiceSource::BusyObserved; }
        }
    }
}
void DciGatewayNode::PrepareProbeGrant(Flow& f,bool startup) {
    auto& a=f.source; auto counters=Port(f.reg.port)->GetProbeCounters(f.queue);
    if (f.closed) return;
    bool grant=startup ? !a.startupGranted :
        !a.recoveryGranted || (counters.remaining<f.reg.frameBytes && counters.sent>a.grantSentBaseline &&
                              a.state[2]>a.grantSeq && a.state[8]>a.lastGrantRx);
    if (!grant) return;
    // The first recovery uses a distinct allowance; subsequent epochs require evidence of reception.
    if (startup) a.startupGranted=true; else a.recoveryGranted=true;
    Add(a.probeEpoch); a.pendingGrant=true; a.lastGrantRx=a.state[8]; a.grantSeq=a.state[2];
    a.grantSentBaseline=counters.sent;
    Event("PROBE_GRANT",f.reg.id,a.probeEpoch,startup ? "startup" : "recovery");
}
void DciGatewayNode::UpdateSource() {
    auto now=Simulator::Now().GetNanoSeconds();
    for (auto& e:m_flows) {
        auto& f=e.second; if (!f.IsSource()) continue;
        auto& a=f.source; auto old=a.mode; a.history.Advance(now);
        a.fresh=a.have && now-int64_t(a.state[3])<=Ns(m_config.timeout);
        a.covered=a.have && a.history.Covers(int64_t(a.state[3])-f.reg.forwardDelay.GetNanoSeconds(),now);
        a.candidate=0; a.mode=TxMode::Stopped; a.reason="not_started";
        f.budget=a.have ? a.state[5] : 0;
        if (a.fresh && a.covered) {
            a.predicted=a.history.Predict(a.state[3],f.reg.forwardDelay.GetNanoSeconds(),now,a.state[4],a.state[6]);
            a.used=m_config.reconstruct ? a.predicted : a.state[4];
        }
        if (f.closed && !f.Queue()) a.reason="closed";
        else if (f.paused) a.reason="local_pause";
        else if (!f.active) a.reason="not_started";
        else if (a.fresh && !(a.state[10]&SENDABLE)) a.reason="remote_pause";
        else if (!a.activeStateReceived && m_config.startupMode=="optimistic") {
            // Pre-start zero STATE is not an active budget. The initial permit is
            // bounded by port capacities, not a probe byte grant; silence still expires.
            if (now-f.reg.start.GetNanoSeconds()>=Ns(m_config.timeout)) a.reason="startup_timeout";
            else {
                a.mode=TxMode::OptimisticStartup; a.reason="optimistic_startup";
                a.candidate=std::min(m_ports.at(f.reg.port).physical,f.reg.peerPortBytesPerSec);
            }
        } else if (!a.activeStateReceived && !f.closed) {
            a.reason="startup"; a.mode=TxMode::StartupProbe;
            PrepareProbeGrant(f,true);
            auto g=Port(f.reg.port)->GetProbeCounters(f.queue);
            if (!f.closed && (a.pendingGrant || g.remaining>=f.reg.frameBytes))
                a.candidate=std::min(m_config.probeRate,m_config.probeBytes/std::max(
                    2*f.reg.forwardDelay.GetSeconds()+m_config.period,m_config.recovery));
        } else if (!a.fresh) a.reason="stale_state";
        else if (!a.covered) a.reason="missing_history";
        else if (!(a.state[10]&SENDABLE)) a.reason="remote_pause";
        else {
            a.mode=TxMode::Normal; a.reason="normal";
            a.candidate=std::min(double(a.state[5]),std::max(0.0,double(a.state[6])-
                std::max(0.0,a.used-a.state[7])/m_config.tau));
            if (!a.candidate && !a.state[6] && a.state[5]>0 && a.state[4]<=a.state[7] &&
                a.used<=a.state[7] && a.predicted<=a.state[7] && !f.closed) {
                a.mode=TxMode::RecoveryProbe; a.reason="zero_service";
                PrepareProbeGrant(f,false);
                auto g=Port(f.reg.port)->GetProbeCounters(f.queue);
                if (a.pendingGrant || g.remaining>=f.reg.frameBytes)
                    a.candidate=std::min(double(a.state[5]),m_config.probeRate);
            }
        }
        if (old!=a.mode) Event("MODE",f.reg.id,uint32_t(a.mode),a.reason);
    }
    for (auto& e:m_ports) {
        auto& p=e.second; if (!m_flows.at(p.flows.front()).IsSource()) continue;
        std::vector<double> caps; size_t demand=0;
        for (auto id:p.flows) {
            auto& f=m_flows.at(id); caps.push_back(f.source.candidate);
            if (f.active) ++demand;
        }
        auto rates=Allocate(caps,p.available);
        for (size_t i=0;i<rates.size();++i) {
            auto& f=m_flows.at(p.flows[i]); f.target=rates[i];
            f.sourceHigh=demand && f.active ? std::floor(m_config.sourceHigh/demand) : 0;
            f.sourceLow=demand && f.active ? std::floor(m_config.sourceLow/demand) : 0;
            if (demand && f.active) {
                if (f.Queue()>f.sourceHigh) f.marking=true;
                else if (f.Queue()<f.sourceLow) f.marking=false;
            } else f.marking=false;
            bool nearDemand=f.upstreamCePending || (f.marking && f.Queue());
            f.nearPending=m_config.nearCnp && nearDemand &&
                (f.lastNearCnp<0 || now-f.lastNearCnp>=Ns(m_config.cnpInterval)) &&
                (f.lastCnp<0 || now-f.lastCnp>=Ns(m_config.cnpInterval));
            if (m_config.nearCnp && nearDemand && !f.nearPending)
                Event("NEAR_CNP_DEFER",f.reg.id,f.Queue(),f.lastNearCnp>=0 &&
                    now-f.lastNearCnp<Ns(m_config.cnpInterval) ? "near_interval" : "native_interval");
        }
    }
}
void DciGatewayNode::BuildReports() {
    auto now=Simulator::Now().GetNanoSeconds();
    std::map<std::pair<uint32_t,uint32_t>,std::vector<Record>> batches;
    for (auto& e:m_flows) {
        auto& f=e.second;
        if (f.lastReport>=0 && now-f.lastReport<Ns(m_config.period) &&
            (!f.dirty || now-f.lastReport<Ns(m_config.reportMin))) continue;
        Add(f.sequence); Add(m_ordinal);
        Record r{}; r[0]=f.reg.id; r[1]=f.reg.generation; r[2]=f.sequence; r[3]=now; r[4]=f.Queue();
        uint64_t flags=(f.active ? HAS_DEMAND : 0)|(f.Queue() ? BACKLOG : 0)|(f.closed ? NO_MORE_HOST_DATA : 0);
        if (f.IsSource()) { r[5]=f.tx; r[6]=flags; r[7]=m_ordinal; }
        else {
            auto& b=f.receiver; const auto& w=b.window.observation;
            r[5]=uint64_t(std::floor(f.budget)); r[6]=uint64_t(std::floor(b.service)); r[7]=b.queueTarget;
            r[8]=f.in; r[9]=f.tx; r[10]=flags|(f.paused ? 0 : SENDABLE); r[11]=uint32_t(b.serviceSource);
            r[12]=w.begin; r[13]=w.end; r[14]=w.busy; r[15]=w.bytes; r[16]=w.pause;
            r[17]=b.lastBudgetChange; r[18]=m_ordinal;
        }
        batches[{f.reg.peer,f.IsSource() ? DEMAND : STATE}].push_back(r);
        f.lastReport=now; f.dirty=false;
        Event(f.IsSource() ? "DEMAND_TX" : "STATE_TX",f.reg.id,f.sequence);
    }
    for (const auto& batch:batches) {
        Ipv4Header ip; ip.SetSource(ControlAddress(GetId())); ip.SetDestination(ControlAddress(batch.first.first));
        ip.SetProtocol(249); ip.SetTtl(64);
        R4Header h; h.kind=batch.first.second;
        size_t perPacket=(m_config.controlIpMtu-ip.GetSerializedSize()-16)/h.RecordBytes();
        NS_ABORT_MSG_IF(!perPacket,"R4 MTU cannot hold record");
        for (size_t begin=0;begin<batch.second.size();begin+=perPacket) {
            auto end=std::min(begin+perPacket,batch.second.size());
            h.records.assign(batch.second.begin()+begin,batch.second.begin()+end);
            auto p=Create<Packet>(); p->AddHeader(h); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
            NS_ABORT_MSG_IF(p->GetSize()>m_config.controlIpMtu,"R4 control MTU exceeded");
            PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); m_reports.push_back(p);
        }
    }
}
bool DciGatewayNode::ApplyRecord(Flow& f,const Record& r,uint32_t kind,const char*& reason) {
    auto now=Simulator::Now().GetNanoSeconds();
    bool state=kind==STATE;
    const auto& old=state ? f.source.state : f.receiver.demand;
    auto flags=r[state ? 10 : 6],ordinal=r[state ? 18 : 7],oldOrdinal=old[state ? 18 : 7];
    reason="identity";
    if (r[0]!=f.reg.id || r[1]!=f.reg.generation || state!=f.IsSource()) return false;
    reason="time_sequence";
    if (!r[2] || r[2]<=old[2] || r[3]>uint64_t(now) || now-int64_t(r[3])>Ns(m_config.timeout) ||
        (old[2] && (r[3]<old[3] || ordinal<oldOrdinal || (r[3]==old[3] && ordinal<=oldOrdinal)))) return false;
    reason="flags";
    if (flags&~uint64_t(HAS_DEMAND|BACKLOG|NO_MORE_HOST_DATA|(state ? SENDABLE : 0)) ||
        bool(flags&BACKLOG)!=(r[4]>0) || (r[4]>0 && !(flags&HAS_DEMAND)) ||
        ((old[state ? 10 : 6]&NO_MORE_HOST_DATA) && !(flags&NO_MORE_HOST_DATA))) return false;
    if (state) {
        reason="state_counters";
        if (r[9]>r[8] || r[8]-r[9]!=r[4] || r[8]<old[8] || r[9]<old[9]) return false;
        reason="state_rates";
        if (r[6]>r[5] || r[5]>f.reg.peerPortBytesPerSec || r[7]>m_config.bufferBytes ||
            r[11]>uint64_t(ServiceSource::BudgetOnly) ||
            (!(flags&SENDABLE) && (r[5] || r[6] || r[11]!=uint64_t(ServiceSource::Paused))) ||
            ((flags&SENDABLE) && r[11]==uint64_t(ServiceSource::Paused))) return false;
        reason="service_window";
        if (r[12]>r[13] || r[13]>r[3] || r[14]>r[13]-r[12] || r[16]>r[13]-r[12] ||
            r[17]>r[3] || r[17]<old[17] || r[15]>r[9]) return false;
        f.source.state=r; f.source.have=true;
        if (r[3]>=uint64_t(f.reg.start.GetNanoSeconds()) && (flags&HAS_DEMAND))
            f.source.activeStateReceived=true;
    } else {
        reason="demand_counters";
        if (r[5]<old[5]) return false;
        f.receiver.demand=r;
        if (flags&NO_MORE_HOST_DATA) f.closed=true;
    }
    f.dirty=true; return true;
}
void DciGatewayNode::ReceiveRecords(Ptr<Packet> packet,const CustomHeader& ch) {
    Add(m_controlRx,packet->GetSize());
    auto reject=[this](uint32_t id,const char* reason) { Add(m_rejected); Event("REJECT",id,m_rejected,reason); };
    auto p=packet->Copy(); PppHeader ppp; Ipv4Header ip;
    if (p->GetSize()<ppp.GetSerializedSize()+20+16 || p->GetSize()>m_config.controlIpMtu+ppp.GetSerializedSize()) {
        reject(0,"packet_length"); return;
    }
    p->RemoveHeader(ppp); p->RemoveHeader(ip); R4Header h;
    if (!p->RemoveHeader(h) || p->GetSize()) { reject(0,"header"); return; }
    std::set<uint64_t> seen;
    for (const auto& r:h.records) {
        if (!seen.insert(r[0]).second) { reject(0,"duplicate_in_packet"); continue; }
        if (r[0]>UINT32_MAX || !m_flows.count(uint32_t(r[0]))) { reject(0,"unknown_flow"); continue; }
        auto& f=m_flows.at(uint32_t(r[0]));
        if (ch.sip!=ControlAddress(f.reg.peer).Get()) { reject(f.reg.id,"peer"); continue; }
        const char* reason="";
        if (!ApplyRecord(f,r,h.kind,reason)) { reject(f.reg.id,reason); continue; }
        Event(h.kind==STATE ? "STATE_RX" : "DEMAND_RX",f.reg.id,r[2]);
    }
}
void DciGatewayNode::CommitControls() {
    for (auto& e:m_ports) {
        std::vector<QbbNetDevice::QueueControl> controls; double sum=0;
        for (auto id:e.second.flows) {
            const auto& f=m_flows.at(id);
            bool probe=f.IsSource() && (f.source.mode==TxMode::StartupProbe || f.source.mode==TxMode::RecoveryProbe);
            controls.push_back({f.queue,f.target,probe}); sum+=f.target;
        }
        NS_ABORT_MSG_IF(sum>e.second.available+1e-5,"R4 targets exceed estimated port capacity");
        Port(e.first)->ApplyQueueControls(controls);
    }
    for (auto& e:m_flows) {
        auto& f=e.second;
        if (f.source.pendingGrant) {
            f.source.pendingGrant=false;
            Port(f.reg.port)->GrantProbeBytes(f.queue,f.source.probeEpoch,m_config.probeBytes);
        }
    }
}
void DciGatewayNode::WriteSamples() {
    auto now=Simulator::Now().GetNanoSeconds();
    for (auto& e:m_flows) {
        auto& f=e.second; auto& a=f.source; auto& b=f.receiver; auto& s=a.state; auto& w=b.window.observation;
        auto g=Port(f.reg.port)->GetProbeCounters(f.queue); Add(m_ordinal);
        m_log << std::setprecision(17) << now << ',' << GetId() << ',' << m_ordinal << ','
            << (f.IsSource() ? "A" : "B") << ',' << f.reg.id << ',' << f.reg.generation << ','
            << f.reg.peer << ',' << f.reg.port << ',' << f.reg.pg << ',' << uint32_t(f.life) << ','
            << f.Queue() << ',' << f.peak << ',' << f.in << ',' << f.tx << ',' << f.rate*8 << ',' << f.budget*8 << ','
            << (f.IsSource() ? s[6] : b.service)*8 << ',' << (f.IsSource() ? s[7] : b.queueTarget) << ','
            << a.predicted << ',' << a.used << ',' << a.candidate*8 << ',' << f.target*8 << ','
            << a.fresh << ',' << a.covered << ',' << uint32_t(a.mode) << ',' << (f.IsSource() ? a.reason : "receiver") << ','
            << s[2] << ',' << s[3] << ',' << s[18] << ',' << s[4] << ',' << s[8] << ',' << s[9] << ','
            << (a.have ? now-int64_t(s[3]) : -1) << ',' << now+f.reg.forwardDelay.GetNanoSeconds() << ',' << f.paused << ','
            << (f.IsSource() ? s[11] : uint64_t(b.serviceSource)) << ','
            << (f.IsSource() ? s[12] : w.begin) << ',' << (f.IsSource() ? s[13] : w.end) << ','
            << (f.IsSource() ? s[14] : w.busy) << ',' << (f.IsSource() ? s[15] : w.bytes) << ','
            << (f.IsSource() ? s[16] : w.pause) << ',' << (f.IsSource() ? s[17] : b.lastBudgetChange) << ','
            << g.epoch << ',' << g.remaining << ',' << g.sent << ',' << f.cnps << ',' << f.decreases << ','
            << f.sourceHigh << ',' << f.sourceLow << ',' << f.marking << ',' << m_controlTx << ',' << m_controlRx << ',' << m_rejected << ',' << m_admissionDropPackets << ','
            << f.upstreamCe << ',' << f.upstreamCePending << ',' << f.nearCnps << ',' << a.activeStateReceived << '\n';
    }
    for (auto& e:m_ports) {
        auto& p=e.second; double target=0; uint64_t sent=0,qTarget=0;
        bool source=m_flows.at(p.flows.front()).IsSource();
        for (auto id:p.flows) { auto& f=m_flows.at(id); target+=f.target; Add(sent,f.tx); Add(qTarget,f.receiver.queueTarget); }
        m_portLog << std::setprecision(17) << now << ',' << GetId() << ',' << e.first << ',' << (source ? "A" : "B")
            << ',' << p.physical << ',' << p.available << ',' << target << ',' << sent << ',' << p.otherTx << ','
            << qTarget << ',' << Port(e.first)->GetProbeTokens() << ',' << Port(e.first)->GetProbeSentBytes() << ','
            << (source ? m_sourceOverload : m_receiverOverload) << '\n';
    }
}
void DciGatewayNode::SendNearCnp(Flow& f) {
    auto p=Create<Packet>(); p->AddHeader(CnHeader(f.reg.sport,f.reg.pg,3,1,1));
    Ipv4Header ip; ip.SetSource(Ipv4Address(f.reg.dip)); ip.SetDestination(Ipv4Address(f.reg.sip));
    ip.SetProtocol(0xff); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); SendNetworkControl(p);
    const char* reason=f.upstreamCePending ? (f.marking && f.Queue() ? "both" : "upstream_ce") : "local_queue";
    f.lastNearCnp=Simulator::Now().GetNanoSeconds(); Add(f.nearCnps);
    Event("NEAR_CNP",f.reg.id,f.Queue(),reason); f.upstreamCePending=false;
}
void DciGatewayNode::Tick() {
    auto now=Simulator::Now().GetNanoSeconds();
    for (auto& e:m_flows) {
        auto& f=e.second;
        if (!f.IsSource()) f.receiver.window.Close(now,Port(f.reg.port)->GetQueuePauseTime(f.reg.pg).GetNanoSeconds());
    }
    SamplePorts(); UpdateDemand(); UpdateReceiver(); UpdateSource();
    BuildReports(); WriteSamples(); CommitControls();
    for (auto p:m_reports) SendNetworkControl(p);
    m_reports.clear();
    for (auto& e:m_flows) {
        auto& f=e.second;
        if (f.nearPending) { f.nearPending=false; SendNearCnp(f); }
        NS_ABORT_MSG_IF(f.Queue()!=Port(f.reg.port)->GetQueue()->GetLogicalBytes(f.queue),"R4 queue accounting mismatch");
    }
    m_tick=Simulator::Schedule(Seconds(m_config.control),&DciGatewayNode::Tick,this);
}
void DciGatewayNode::WriteMetadata(std::ostream& out) const {
    out << "{\"node\":" << GetId() << ",\"schema_version\":1,\"max_flows_per_port\":120,\"buffer_bytes\":"
        << m_config.bufferBytes << ",\"buffer_high_fraction\":0.8,\"buffer_low_fraction\":0.5,\"flow_state_bytes\":"
        << sizeof(Flow) << ",\"fixed_frame_delay_assumption\":true,\"flows\":["; bool comma=false;
    for (const auto& e:m_flows) {
        const auto& f=e.second; const auto& r=f.reg;
        out << (comma ? "," : "") << "{\"id\":" << r.id << ",\"generation\":" << r.generation << ",\"role\":\""
            << (f.IsSource() ? "A" : "B") << "\",\"peer\":" << r.peer << ",\"port\":" << r.port << ",\"pg\":" << r.pg
            << ",\"logical_queue\":" << f.queue << ",\"frame_bytes\":" << r.frameBytes
            << ",\"forward_delay_ns\":" << r.forwardDelay.GetNanoSeconds() << ",\"feedback_rtt_ns\":" << r.feedbackRtt.GetNanoSeconds()
            << ",\"start_ns\":" << r.start.GetNanoSeconds() << ",\"peer_port_Bps\":" << r.peerPortBytesPerSec
            << ",\"history_buckets\":" << f.source.history.bins.size()
            << ",\"history_bytes\":" << f.source.history.bins.size()*sizeof(uint64_t) << "}"; comma=true;
    }
    out << "]}";
}
void DciGatewayNode::DoDispose() {
    Simulator::Cancel(m_tick);
    if (m_started) for (const auto& e:m_ports) {
        auto d=Port(e.first); d->m_groupClassifier=Callback<uint32_t,Ptr<const Packet>,uint32_t>();
        d->TraceDisconnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,this,e.first));

    }
    if (m_started) for (uint32_t i=0;i<GetNDevices();++i) {
        auto d=DynamicCast<QbbNetDevice>(GetDevice(i));
        if (d) d->TraceDisconnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,this,i));
    }
    m_flows.clear(); m_ports.clear(); m_reports.clear(); m_data.clear(); m_feedback.clear();
    m_log.close(); m_portLog.close(); m_events.close(); m_packets.close(); SwitchNode::DoDispose();
}
}
