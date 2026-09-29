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
constexpr uint64_t ACTIVE=1, BACKLOG=2, CLOSED=4, PAUSED=8;
constexpr uint64_t STATE=1, DEMAND=2;
constexpr uint32_t WORDS=14, RECORD_WORDS=4, PER_FRAGMENT=24;
// At most 24 records per frame: 112 + 768 + 22 = 902 bytes.
class R3Header : public Header {
public:
    std::array<uint64_t,WORDS> words{};
    std::vector<std::array<uint64_t,RECORD_WORDS>> records;
    static TypeId GetTypeId() {
        static TypeId tid=TypeId("ns3::R3Header").SetParent<Header>().AddConstructor<R3Header>();
        return tid;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize() const override { return WORDS*8+records.size()*RECORD_WORDS*8; }
    void Serialize(Buffer::Iterator i) const override {
        for (auto w:words) i.WriteHtonU64(w);
        for (const auto& r:records) for (auto w:r) i.WriteHtonU64(w);
    }
    uint32_t Deserialize(Buffer::Iterator i) override {
        if (i.GetRemainingSize()<WORDS*8) return 0;
        for (auto& w:words) w=i.ReadNtohU64();
        if (words[13]>PER_FRAGMENT || i.GetRemainingSize()!=words[13]*RECORD_WORDS*8) return 0;
        records.resize(words[13]);
        for (auto& r:records) for (auto& w:r) w=i.ReadNtohU64();
        return GetSerializedSize();
    }
    void Print(std::ostream& o) const override { o << "R3 seq=" << words[4]; }
};

}
TypeId DciGatewayNode::GetTypeId() {
    static TypeId tid=TypeId("ns3::DciGatewayNode").SetParent<SwitchNode>()
        .AddConstructor<DciGatewayNode>();
    return tid;
}
Ipv4Address DciGatewayNode::ControlAddress(uint32_t node) { return Ipv4Address(0xac1f0000u+node+1); }
Ptr<QbbNetDevice> DciGatewayNode::Port(uint32_t port) const {
    auto dev=DynamicCast<QbbNetDevice>(GetDevice(port));
    NS_ABORT_MSG_IF(!dev,"R3 requires a Qbb port"); return dev;
}
void DciGatewayNode::Configure(const Config& c,const std::string& output) {
    NS_ABORT_MSG_IF(m_enabled,"R3 configured twice");
    NS_ABORT_MSG_IF(!std::isfinite(c.period+c.control+c.bin+c.reaction+c.gamma+c.recovery+
        c.probeRate+c.increase+c.utilization+c.qref+c.tau+c.timeout+c.fallback+c.sourceHigh+
        c.sourceLow+c.cnpInterval+c.reportMin) || c.period<1e-9 || c.control<1e-9 || c.bin<1e-9 ||
        c.reaction<c.control || c.recovery<c.control || c.gamma<=0 || c.gamma>=1 || c.probeRate<=0 ||
        c.increase<=0 || c.utilization<=0 || c.utilization>1 || c.qref<0 || c.tau<=0 ||
        c.timeout<=c.period || c.fallback<=0 || c.fallback>1 || c.sourceLow<=0 ||
        c.sourceHigh<=c.sourceLow || c.sourceHigh>=c.bufferBytes || c.cnpInterval<=0 || c.reportMin<c.control || c.period<c.reportMin,
        "invalid R3 configuration");
    m_config=c; m_enabled=true;
    const auto prefix=output+".gateway-"+std::to_string(GetId());
    m_log.open(prefix+".csv"); m_events.open(prefix+".events.csv"); m_packets.open(prefix+".packets.csv");
    NS_ABORT_MSG_IF(!m_log || !m_events || !m_packets,"cannot open R3 logs");
    m_log << "time_s,node,role,group,flow,generation,queue_bytes,peak_group_bytes,in_bytes,tx_bytes,alpha,reaction_bps,budget_bps,target_bps,service_bps,predicted_queue_bytes,snapshot_seq,snapshot_age_s,active,cnp_count,decrease_count,control_tx_bytes,control_rx_bytes,rejected,unmatched_cnp,admission_drops,snapshot_time_s,snapshot_queue_bytes,prediction_end_s,queue_used_bytes,group_target_bps,pre_port_target_bps,paused,snapshot_valid,reconstruct\n";
    m_events << "time_s,event,object,value\n";
    m_packets << "time_ns,role,group,flow,bytes,queue_bytes\n";
}
void DciGatewayNode::RegisterFlow(const Registration& r) {
    NS_ABORT_MSG_IF(!m_enabled || m_flows.count(r.id) || !r.generation || r.pg==0 || r.pg>=8 ||
        r.delay<=0 || r.feedbackRtt<=0 || r.burst>=m_config.probeBytes,"invalid R3 registration");
    DataKey key{r.sip,r.dip,r.sport,r.dport,r.pg};
    FeedbackKey feedback{r.dip,r.sip,r.sport,r.pg};
    NS_ABORT_MSG_IF(m_data.count(key) || m_feedback.count(feedback),"ambiguous/reused R3 QP wire identity");
    m_data[key]=r.id; m_feedback[feedback]=r.id;
    auto& next=m_nextQueue[r.port]; if (!next) next=QbbNetDevice::qCnt;
    NS_ABORT_MSG_IF(next>=BEgressQueue::fCnt,"R3 exceeds 120 registered flows per egress port");
    auto& f=m_flows[r.id]; f.reg=r; f.queue=next++; f.rate=m_config.probeRate;
    auto& g=m_groups[r.group];
    if (g.flows.empty()) {
        g.id=r.group; g.peer=r.peer; g.port=r.port; g.pg=r.pg; g.source=r.source; g.delay=r.delay;
        if (r.source) g.history.Init(m_config.bin,m_config.timeout+2*r.delay+4*m_config.bin);
    }
    NS_ABORT_MSG_IF(g.peer!=r.peer || g.port!=r.port || g.pg!=r.pg || g.source!=r.source,
                    "inconsistent R3 group mapping");
    g.feedbackRtt=std::max(g.feedbackRtt,r.feedbackRtt); g.flows.push_back(r.id);
    Port(r.port)->ConfigureGroupShaper(f.queue,r.pg,0,r.burst);
}
void DciGatewayNode::Start() {
    if (!m_enabled) return;
    for (const auto& entry:m_nextQueue) {
        auto d=Port(entry.first);
        d->m_groupClassifier=MakeBoundCallback(&DciGatewayNode::Classify,this,entry.first);
        d->TraceConnectWithoutContext("QbbEnqueue",MakeBoundCallback(&DciGatewayNode::Enqueue,this,entry.first));
        d->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,this,entry.first));
    }
    m_tick=Simulator::Schedule(Seconds(m_config.control),&DciGatewayNode::Tick,this);
}
void DciGatewayNode::CompleteFlow(uint32_t id) {
    auto it=m_flows.find(id); if (it!=m_flows.end() && it->second.reg.source) it->second.closed=true;
}
DciGatewayNode::Flow* DciGatewayNode::Match(Ptr<const Packet> p) {
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(h);
    if (h.l3Prot!=0x11) return nullptr;
    auto it=m_data.find({h.sip,h.dip,h.udp.sport,h.udp.dport,h.udp.pg});
    return it==m_data.end() ? nullptr : &m_flows.at(it->second);
}
bool DciGatewayNode::ShouldMarkEcn(uint32_t port, Ptr<const Packet> packet) {
    if (!m_enabled || m_config.shaperEcn) return true;
    auto flow=Match(packet);
    // Only omit NEW marks from registered shaping queues. Existing ECN bits
    // survive, and unregistered traffic/other egresses retain ordinary ECN.
    return !flow || flow->reg.port!=port;
}
uint32_t DciGatewayNode::Classify(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t pg) {
    auto f=self->Match(p); return f && f->reg.port==port ? f->queue : pg;
}
void DciGatewayNode::Enqueue(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t) {
    auto f=self->Match(p); if (!f || f->reg.port!=port) return;
    double now=Simulator::Now().GetSeconds();
    if (f->lastSeen<0 || (f->in==f->tx && now-f->lastSeen>self->m_config.timeout)) {
        f->probing=true; f->rate=self->m_config.probeRate; f->recoverySince=now;
    }
    f->in+=p->GetSize(); f->lastSeen=now;
    auto& g=self->m_groups.at(f->reg.group); g.peak=std::max(g.peak,self->Queue(g));
}
void DciGatewayNode::Dequeue(DciGatewayNode* self,uint32_t port,Ptr<const Packet> p,uint32_t) {
    auto f=self->Match(p); if (!f || f->reg.port!=port) {self->m_otherTx[port]+=p->GetSize(); return;}
    f->tx+=p->GetSize(); NS_ABORT_MSG_IF(f->tx>f->in,"R3 byte conservation failed");
    if (f->reg.source) self->m_groups.at(f->reg.group).history.Add(Simulator::Now().GetNanoSeconds(),p->GetSize());
    self->m_packets << Simulator::Now().GetNanoSeconds() << ',' << (f->reg.source ? "A" : "B")
        << ',' << f->reg.group << ',' << f->reg.id << ',' << p->GetSize() << ',' << f->in-f->tx << '\n';
}
uint64_t DciGatewayNode::Queue(const Group& g) const {
    uint64_t q=0; for (auto id:g.flows) {const auto& f=m_flows.at(id); q+=f.in-f.tx;} return q;
}
void DciGatewayNode::Event(const char* name,uint32_t id,double value) {
    m_events << std::setprecision(15) << Simulator::Now().GetSeconds() << ',' << name << ',' << id << ',' << value << '\n';
}
bool DciGatewayNode::SwitchReceiveFromDevice(Ptr<NetDevice> dev,Ptr<Packet> p,CustomHeader& h) {
    if (m_enabled) {
        if (h.l3Prot==249 && h.dip==ControlAddress(GetId()).Get()) {ReceiveSnapshot(p,h); return true;}
        ObserveFeedback(h);
    }
    return SwitchNode::SwitchReceiveFromDevice(dev,p,h);
}
void DciGatewayNode::ObserveFeedback(CustomHeader& h) {
    bool cnp=h.l3Prot==0xff && h.cnp.ecnBits!=0;
    bool ack=(h.l3Prot==0xfc || h.l3Prot==0xfd) && (h.ack.flags&(1<<qbbHeader::FLAG_CNP));
    if (!cnp && !ack) return;
    auto it=m_feedback.find({h.sip,h.dip,cnp ? h.cnp.fid : h.ack.dport,cnp ? h.cnp.qIndex : h.ack.pg});
    if (it==m_feedback.end() || (ack && m_flows.at(it->second).reg.dport!=h.ack.sport)) {
        ++m_unmatched; Event("UNMATCHED_CNP",0,m_unmatched); return;
    }
    auto& f=m_flows.at(it->second); f.lastCnp=Simulator::Now().GetSeconds(); ++f.cnps;
    // A observes but never applies another multiplicative decrease.
    if (!f.reg.source) f.event=true;
}
std::vector<double> DciGatewayNode::Allocate(const std::vector<double>& caps,double capacity) {
    std::vector<double> sorted=caps, result; std::sort(sorted.begin(),sorted.end());
    double remaining=std::max(0.0,capacity), level=0;
    for (size_t i=0;i<sorted.size();++i) {
        level=remaining/(sorted.size()-i);
        if (sorted[i]>=level) break;
        remaining-=sorted[i];
    }
    for (double cap:caps) result.push_back(std::min(cap,level));
    return result;
}
void DciGatewayNode::History::Init(double w,double retention) {
    width=Seconds(w).GetNanoSeconds(); NS_ABORT_MSG_IF(width<=0,"R3 history bin below 1 ns");
    bins.assign(size_t(std::ceil(retention/w))+2,0);
}
void DciGatewayNode::History::Advance(int64_t now) {
    int64_t next=now/width; NS_ABORT_MSG_IF(next<latest,"R3 history time reversed");
    if (next-latest>=int64_t(bins.size())) std::fill(bins.begin(),bins.end(),0);
    else for (int64_t b=latest+1;b<=next;++b) bins[b%bins.size()]=0;
    latest=next;
}
void DciGatewayNode::History::Add(int64_t now,uint32_t bytes) {Advance(now); bins[latest%bins.size()]+=bytes;}
bool DciGatewayNode::History::Covers(int64_t begin,int64_t end) const {
    return end>=begin && std::max(int64_t(0),begin)/width>=std::max(int64_t(0),latest-int64_t(bins.size())+1);
}
double DciGatewayNode::History::Predict(int64_t sample,int64_t delay,int64_t now,double q,double service) const {
    int64_t begin=sample-delay;
    NS_ABORT_MSG_IF(!Covers(begin,now),"R3 missing history");
    for (int64_t t=begin;t<now;) {
        int64_t next=std::min(now,t<0 ? int64_t(0) : (t/width+1)*width);
        double bytes=0;
        if (t>=0) {
            int64_t base=t/width*width, end=std::min(base+width,now);
            if (end>base) bytes=double(bins[(t/width)%bins.size()])*(next-t)/(end-base);
        }
        q=std::max(0.0,q+bytes-service*(next-t)*1e-9); t=next;
    }
    return q;
}
void DciGatewayNode::UpdateReceiver() {
    const double now=Simulator::Now().GetSeconds();
    uint64_t buffered=0;
    for (const auto& entry:m_groups) if (!entry.second.source) buffered+=Queue(entry.second);
    if (!m_receiverOverload && buffered>0.8*m_config.bufferBytes) {
        m_receiverOverload=true; Event("B_BUFFER_HIGH",GetId(),buffered);
    } else if (m_receiverOverload && buffered<0.5*m_config.bufferBytes) {
        m_receiverOverload=false; Event("B_BUFFER_LOW",GetId(),buffered);
    }
    std::map<uint32_t,std::vector<uint32_t>> byPort;
    for (auto& entry:m_flows) {
        auto& f=entry.second; if (f.reg.source) continue;
        auto& g=m_groups.at(f.reg.group); auto dev=Port(f.reg.port);
        f.active=f.in>f.tx || (!f.closed && f.demand && now-g.demandTime<=m_config.timeout) ||
            (f.lastSeen>=0 && now-f.lastSeen<=2*g.delay+g.feedbackRtt);
        if (now-f.window+1e-12>=m_config.reaction) {
            f.alpha=(1-m_config.gamma)*f.alpha+m_config.gamma*(f.event ? 1 : 0);
            if (f.event) {
                f.rate=std::max(m_config.probeRate,f.rate*(1-f.alpha/2)); ++f.decreases;
                f.recoverySince=now; g.dirty=true; Event("B_DECREASE",f.reg.id,f.rate*8);
            }
            f.event=false; f.window=now;
        }
        const double wait=std::max(m_config.recovery,g.feedbackRtt);
        if (dev->IsQueuePaused(f.reg.pg) || now-f.lastCnp<wait) {
            f.recoverySince=now; f.previousTx=f.tx;
        } else if (now-f.recoverySince+1e-12>=wait) {
            if (f.tx>f.previousTx && f.lastSeen>=0 && now-f.lastSeen<=wait) {
                f.rate=std::min(dev->GetDataRate().GetBitRate()/8.0,f.rate+m_config.increase);
                f.probing=false; Event("B_RECOVER",f.reg.id,f.rate*8);
            }
            f.recoverySince=now; f.previousTx=f.tx;
        }
        byPort[f.reg.port].push_back(f.reg.id);
    }
    // Joint allocation across PGs prevents issuing the full physical capacity to each PG.
    for (const auto& entry:byPort) {
        auto dev=Port(entry.first); std::vector<double> caps;
        for (auto id:entry.second) {
            const auto& f=m_flows.at(id);
            double cap=f.rate;
            if (f.probing) cap=std::min(cap,(m_config.probeBytes-f.reg.burst)/std::max(f.reg.feedbackRtt,m_config.recovery));
            caps.push_back(f.active && !dev->IsQueuePaused(f.reg.pg) ? cap : 0);
        }
        // Uncontrolled traffic shares the real scheduler. Its last-interval service
        // is subtracted; this is an estimate, not a guaranteed residual capacity.
        double capacity=std::max(0.0,dev->GetDataRate().GetBitRate()/8.0*m_config.utilization-
                                 m_otherTx[entry.first]/m_config.control);
        if (m_receiverOverload) capacity*=m_config.fallback;
        auto rates=Allocate(caps,capacity);
        for (size_t i=0;i<rates.size();++i) {
            auto& f=m_flows.at(entry.second[i]); f.budget=rates[i]; f.target=rates[i];
            dev->SetGroupShaperRate(f.queue,f.target);
        }
    }
    for (auto& entry:m_groups) {
        auto& g=entry.second; if (g.source) continue;
        g.budget=0; g.service=0;
        const bool backlog=Queue(g)>0;
        for (auto id:g.flows) {
            const auto& f=m_flows.at(id); g.budget+=f.budget;
            // Empty queues do not prove zero capacity; when backlog exists,
            // only backlogged flows contribute to its drain service estimate.
            if (!backlog || f.in>f.tx) g.service+=f.budget;
        }
        if (Port(g.port)->IsQueuePaused(g.pg)) g.service=0;
    }
}
void DciGatewayNode::UpdateSource() {
    const double now=Simulator::Now().GetSeconds(); const int64_t nowNs=Simulator::Now().GetNanoSeconds();
    std::map<uint32_t,std::vector<uint32_t>> byPort;
    std::map<uint32_t,double> desired;
    for (auto& entry:m_groups) {
        auto& g=entry.second; if (!g.source) continue;
        g.history.Advance(nowNs);
        bool fresh=g.have && now-g.snapshot.sample*1e-9<=m_config.timeout &&
            g.history.Covers(int64_t(g.snapshot.sample)-Seconds(g.delay).GetNanoSeconds(),nowNs);
        g.fresh=fresh;
        double sum=0;
        for (auto id:g.flows) {
            auto& f=m_flows.at(id); f.active=f.in>f.tx || (!f.closed && now>=f.reg.start);
            if (!g.have) f.budget=std::min(m_config.probeRate,
                (m_config.probeBytes-f.reg.burst)/std::max(2*g.delay+m_config.period,m_config.recovery));
            sum+=f.active ? f.budget : 0;
        }
        double cap=sum;
        if (fresh) {
            g.predicted=g.history.Predict(g.snapshot.sample,Seconds(g.delay).GetNanoSeconds(),nowNs,
                                          g.snapshot.queue,g.snapshot.service);
            g.queueUsed=m_config.reconstruct ? g.predicted : double(g.snapshot.queue);
            cap=std::min({sum,double(g.snapshot.budget),std::max(0.0,double(g.snapshot.service)-
                 std::max(0.0,g.queueUsed-m_config.qref)/m_config.tau)});
        } else if (g.have) {
            // Stale/incomplete reports never authorize a rate increase.
            cap=std::min(g.target,Port(g.port)->GetDataRate().GetBitRate()/8.0*m_config.fallback);
        }
        g.unconstrained=cap;
        desired[g.id]=cap; byPort[g.port].push_back(g.id);
    }
    for (const auto& entry:byPort) {
        auto dev=Port(entry.first); std::vector<double> caps;
        for (auto id:entry.second) caps.push_back(dev->IsQueuePaused(m_groups.at(id).pg) ? 0 : desired.at(id));
        auto shares=Allocate(caps,std::max(0.0,dev->GetDataRate().GetBitRate()/8.0*m_config.utilization-
                                                    m_otherTx[entry.first]/m_config.control));
        for (size_t i=0;i<shares.size();++i) {
            auto& g=m_groups.at(entry.second[i]); caps.clear();
            for (auto id:g.flows) {const auto& f=m_flows.at(id); caps.push_back(f.active ? f.budget : 0);}
            auto rates=Allocate(caps,shares[i]); g.target=std::accumulate(rates.begin(),rates.end(),0.0);
            if (Queue(g)>m_config.sourceHigh) g.marking=true;
            else if (Queue(g)<m_config.sourceLow) g.marking=false;
            for (size_t j=0;j<rates.size();++j) {
                auto& f=m_flows.at(g.flows[j]); f.target=rates[j]; dev->SetGroupShaperRate(f.queue,f.target);
                if (g.marking && f.in>f.tx && now-f.lastNearCnp>=m_config.cnpInterval &&
                    now-f.lastCnp>=m_config.cnpInterval) SendNearCnp(f);
            }
        }
    }
}
void DciGatewayNode::SendNearCnp(Flow& f) {
    auto p=Create<Packet>(); p->AddHeader(CnHeader(f.reg.sport,f.reg.pg,3,1,1));
    Ipv4Header ip; ip.SetSource(Ipv4Address(f.reg.dip)); ip.SetDestination(Ipv4Address(f.reg.sip));
    ip.SetProtocol(0xff); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); SendNetworkControl(p);
    f.lastNearCnp=Simulator::Now().GetSeconds(); Event("NEAR_CNP",f.reg.id,f.in-f.tx);
}
void DciGatewayNode::SendSnapshot(Group& g) {
    Snapshot s; s.kind=g.source ? DEMAND : STATE; s.group=g.id; s.seq=++g.sequence;
    s.sample=Simulator::Now().GetNanoSeconds(); s.queue=Queue(g); s.service=g.service; s.budget=g.budget;
    s.flags=Port(g.port)->IsQueuePaused(g.pg) ? PAUSED : 0;
    for (auto id:g.flows) {
        const auto& f=m_flows.at(id);
        s.records.push_back({id,f.reg.generation,uint64_t(f.budget),
            (f.active ? ACTIVE : 0)|(f.in>f.tx ? BACKLOG : 0)|(f.closed ? CLOSED : 0)});
    }
    uint32_t parts=(s.records.size()+PER_FRAGMENT-1)/PER_FRAGMENT;
    for (uint32_t part=0;part<parts;++part) {
        R3Header h; uint32_t end=std::min<size_t>((part+1)*PER_FRAGMENT,s.records.size());
        h.words={3,s.kind,s.group,s.epoch,s.seq,s.sample,s.queue,s.service,s.budget,s.flags,
                 part,parts,s.records.size(),end-part*PER_FRAGMENT};
        for (uint32_t j=part*PER_FRAGMENT;j<end;++j) {
            const auto& r=s.records[j]; h.records.push_back({r.id,r.generation,r.rate,r.flags});
        }
        auto p=Create<Packet>(); p->AddHeader(h);
        Ipv4Header ip; ip.SetSource(ControlAddress(GetId())); ip.SetDestination(ControlAddress(g.peer));
        ip.SetProtocol(249); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
        m_controlTx+=p->GetSize(); SendNetworkControl(p);
    }
    g.lastSend=Simulator::Now().GetSeconds(); g.dirty=false; Event(g.source ? "DEMAND_TX" : "STATE_TX",g.id,s.seq);
}
void DciGatewayNode::ReceiveSnapshot(Ptr<Packet> packet,const CustomHeader& ch) {
    m_controlRx+=packet->GetSize();
    auto p=packet->Copy(); PppHeader ppp; Ipv4Header ip; p->RemoveHeader(ppp); p->RemoveHeader(ip);
    R3Header h;
    if (p->RemoveHeader(h)==0 || p->GetSize()!=0) {++m_rejected; return;}
    const auto& w=h.words; auto it=m_groups.find(w[2]);
    if (w[0]!=3 || w[3]!=1 || it==m_groups.end() || w[5]>uint64_t(Simulator::Now().GetNanoSeconds())) {
        ++m_rejected; return;
    }
    auto& g=it->second;
    if (ch.sip!=ControlAddress(g.peer).Get() || w[1]!=(g.source ? STATE : DEMAND) ||
        w[12]!=g.flows.size() || w[11]!=(g.flows.size()+PER_FRAGMENT-1)/PER_FRAGMENT || w[10]>=w[11] ||
        w[13]!=std::min<uint64_t>(PER_FRAGMENT,w[12]-w[10]*PER_FRAGMENT) ||
        w[4]<=(g.source ? g.snapshot.seq : g.demandSeq) ||
        (Simulator::Now().GetNanoSeconds()-w[5])*1e-9>m_config.timeout) {++m_rejected; return;}
    auto& a=g.assembly;
    if (w[4]<a.value.seq) {++m_rejected; return;}
    if (w[4]>a.value.seq) {
        a=Assembly{}; a.parts=w[11];
        a.value.kind=w[1]; a.value.group=w[2]; a.value.epoch=w[3]; a.value.seq=w[4];
        a.value.sample=w[5]; a.value.queue=w[6]; a.value.service=w[7]; a.value.budget=w[8]; a.value.flags=w[9];
    }
    if (a.value.sample!=w[5] || a.value.queue!=w[6] || a.value.service!=w[7] ||
        a.value.budget!=w[8] || a.value.flags!=w[9] || a.parts!=w[11] || a.fragments.count(w[10])) {
        ++m_rejected; return;
    }
    std::vector<Record> records;
    for (const auto& r:h.records) {
        auto f=m_flows.find(r[0]);
        if (f==m_flows.end() || f->second.reg.group!=g.id || r[1]!=f->second.reg.generation ||
            r[2]>Port(g.port)->GetDataRate().GetBitRate()/8.0 || (r[3]&~uint64_t(ACTIVE|BACKLOG|CLOSED))) {
            ++m_rejected; return;
        }
        records.push_back({r[0],r[1],r[2],r[3]});
    }
    a.fragments[w[10]]=records;
    if (a.fragments.size()!=a.parts) return;
    std::set<uint64_t> ids;
    for (const auto& fragment:a.fragments) for (const auto& r:fragment.second) {
        if (!ids.insert(r.id).second) {++m_rejected; a.fragments.clear(); return;}
        a.value.records.push_back(r);
    }
    if (ids.size()!=g.flows.size()) {++m_rejected; a.fragments.clear(); return;}
    if (g.source) {
        if (g.have && a.value.sample<g.snapshot.sample) {++m_rejected; return;}
        g.snapshot=a.value; g.have=true;
        for (const auto& r:g.snapshot.records) m_flows.at(r.id).budget=r.rate;
    } else {
        g.demandSeq=a.value.seq; g.demandTime=a.value.sample*1e-9;
        for (const auto& r:a.value.records) {
            auto& f=m_flows.at(r.id); f.demand=r.flags&(ACTIVE|BACKLOG); f.closed=r.flags&CLOSED;
        }
    }
    Event(g.source ? "STATE_APPLIED" : "DEMAND_APPLIED",g.id,a.value.seq);
}
void DciGatewayNode::Tick() {
    UpdateReceiver(); UpdateSource();
    const double now=Simulator::Now().GetSeconds();
    for (auto& entry:m_groups) {
        auto& g=entry.second;
        if (now-g.lastSend+1e-12>=m_config.period || (g.dirty && now-g.lastSend+1e-12>=m_config.reportMin)) SendSnapshot(g);
        for (auto id:g.flows) {
            const auto& f=m_flows.at(id);
            NS_ABORT_MSG_IF(f.in-f.tx!=Port(g.port)->GetQueue()->GetLogicalBytes(f.queue),"R3 logical queue/MMU path mismatch");
            m_log << std::setprecision(15) << now << ',' << GetId() << ',' << (g.source ? "A" : "B") << ','
                << g.id << ',' << id << ',' << f.reg.generation << ',' << f.in-f.tx << ',' << g.peak << ','
                << f.in << ',' << f.tx << ',' << f.alpha << ',' << f.rate*8 << ',' << f.budget*8 << ','
                << f.target*8 << ',' << (g.source ? g.snapshot.service : g.service)*8 << ',' << g.predicted << ','
                << g.snapshot.seq << ',' << (g.have ? now-g.snapshot.sample*1e-9 : -1) << ',' << f.active << ','
                << f.cnps << ',' << f.decreases << ',' << m_controlTx << ',' << m_controlRx << ',' << m_rejected << ','
                << m_unmatched << ',' << m_admissionDropPackets << ',' << g.snapshot.sample*1e-9
                << ',' << g.snapshot.queue << ',' << now+g.delay << ',' << g.queueUsed
                << ',' << g.target*8 << ',' << g.unconstrained*8 << ',' << Port(g.port)->IsQueuePaused(g.pg)
                << ',' << g.fresh << ',' << m_config.reconstruct << '\n';
        }
    }
    m_otherTx.clear();
    m_tick=Simulator::Schedule(Seconds(m_config.control),&DciGatewayNode::Tick,this);
}
void DciGatewayNode::WriteMetadata(std::ostream& out) const {
    out << "{\"node\":" << GetId() << ",\"groups\":["; bool comma=false;
    for (const auto& entry:m_groups) {
        const auto& g=entry.second; out << (comma ? "," : "") << "{\"id\":" << g.id << ",\"role\":\""
            << (g.source ? "A" : "B") << "\",\"peer\":" << g.peer << ",\"port\":" << g.port
            << ",\"pg\":" << g.pg << ",\"forward_delay_s\":" << g.delay << ",\"feedback_rtt_s\":" << g.feedbackRtt
            << ",\"flows\":["; bool first=true;
        for (auto id:g.flows) {out << (first ? "" : ",") << id; first=false;}
        out << "]}"; comma=true;
    }
    out << "]}";
}
void DciGatewayNode::DoDispose() {
    Simulator::Cancel(m_tick);
    for (const auto& entry:m_nextQueue) {
        auto d=Port(entry.first); d->m_groupClassifier=Callback<uint32_t,Ptr<const Packet>,uint32_t>();
        d->TraceDisconnectWithoutContext("QbbEnqueue",MakeBoundCallback(&DciGatewayNode::Enqueue,this,entry.first));
        d->TraceDisconnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,this,entry.first));
    }
    m_flows.clear(); m_groups.clear(); m_log.close(); m_events.close(); m_packets.close();
    SwitchNode::DoDispose();
}
}
