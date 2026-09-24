// Proposed R2: receiver STATE, source byte-bin history, one rate formula and FIFO shaper.
#include <cmath>
#include <iomanip>
#include <tuple>
#include <memory>
#include "ns3/cn-header.h"
#include "ns3/ppp-header.h"

namespace Proposed {
class StateHeader : public Header {
public:
    // B, B/s and ns, network byte order. WAN_RATE is used only by the receiver
    // extrapolation ablation; all modes send identical-size periodic STATEs.
    enum Field { VERSION, GROUP, EPOCH, SEQ, SAMPLE, QUEUE, SERVICE, EXTERNAL,
                 BUDGET, WAN_RATE, COUNT };
    uint64_t v[COUNT] = {};
    static TypeId GetTypeId() {
        static TypeId id = TypeId("ns3::ProposedStateHeader").SetParent<Header>()
            .AddConstructor<StateHeader>();
        return id;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize() const override { return COUNT * 8; }
    void Serialize(Buffer::Iterator i) const override { for (auto x:v) i.WriteHtonU64(x); }
    uint32_t Deserialize(Buffer::Iterator i) override {
        for (auto &x:v) x=i.ReadNtohU64();
        return GetSerializedSize();
    }
    void Print(std::ostream &s) const override { s << "STATE seq=" << v[SEQ]; }
};

// Fixed-size ring of actual sent bytes. Empty elapsed bins are explicitly zeroed.
// Partial boundary bins assume uniform arrival over their observed portion.
class ByteHistory {
public:
    std::vector<uint64_t> bytes;
    int64_t width=1, latest=0;
    void Configure(int64_t binNs, size_t count) {
        NS_ABORT_MSG_IF(binNs<=0 || count<2,"invalid Proposed history size");
        width=binNs; latest=0; bytes.assign(count,0);
    }
    void Advance(int64_t now) {
        int64_t next=now/width;
        NS_ABORT_MSG_IF(next<latest,"history time moved backwards");
        if (next-latest>=int64_t(bytes.size())) std::fill(bytes.begin(),bytes.end(),0);
        else for (int64_t b=latest+1;b<=next;++b) bytes[b%bytes.size()]=0;
        latest=next;
    }
    void Add(int64_t now,uint32_t size) { Advance(now); bytes[latest%bytes.size()]+=size; }
    bool Covers(int64_t begin,int64_t end,int64_t known) const {
        return end<=known && end>=begin &&
            std::max(int64_t(0),begin)/width>=std::max(int64_t(0),latest-int64_t(bytes.size())+1);
    }
    double Segment(int64_t begin,int64_t end,int64_t known) const {
        if (end<=0) return 0; // pre-simulation history is known to be empty
        NS_ABORT_MSG_IF(!Covers(begin,end,known),"missing/future Proposed history");
        begin=std::max(int64_t(0),begin);
        int64_t b=begin/width;
        int64_t observedEnd=std::min((b+1)*width,known);
        NS_ABORT_MSG_IF(end>observedEnd,"history segment crosses bin boundary");
        return observedEnd>b*width ? double(bytes[b%bytes.size()])*(end-begin)/(observedEnd-b*width) : 0;
    }
};

uint32_t predictor=0;
std::string output;
double period=0.0002, delta=0.00005, binWidth=0.00005;
double qref=0, tau=0, sourceHigh=250000, sourceLow=62500;
double utilization=0.98, wanFraction=1, epsilon=0.05, cnpInterval=0.00005;
bool enabled=false;
class Group;
double SourceShare(const Group &group,double now);
std::vector<std::unique_ptr<Group>> groups;
std::ofstream log, events, packets, receiverLog, receiverEvents;
Ipv4Address ControlAddress(uint32_t node) { return Ipv4Address(0xac1f0000u+node+1); }
Ptr<Packet> RoutePacket(Ipv4Address src,Ipv4Address dst,uint8_t protocol) {
    auto p=Create<Packet>(); Ipv4Header ip; ip.SetSource(src); ip.SetDestination(dst);
    ip.SetProtocol(protocol); ip.SetTtl(64); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); return p;
}
uint32_t Peer(Ptr<QbbNetDevice> dev) {
    auto ch=dev->GetChannel();
    return ch->GetDevice(0)==dev ? ch->GetDevice(1)->GetNode()->GetId() : ch->GetDevice(0)->GetNode()->GetId();
}
std::vector<uint32_t> TracePath(uint32_t start,uint32_t end,Ptr<Packet> p,CustomHeader h) {
    std::vector<uint32_t> path{start};
    while (path.back()!=end) {
        auto node=n.Get(path.back()); Ptr<QbbNetDevice> dev;
        if (!node->GetNodeType()) {
            NS_ABORT_MSG_IF(nbr2if[node].size()!=1,"multihomed host needs explicit route selection");
            dev=DynamicCast<QbbNetDevice>(node->GetDevice(nbr2if[node].begin()->second.idx));
        } else {
            int port=DynamicCast<SwitchNode>(node)->LookupOutputPort(p,h);
            NS_ABORT_MSG_IF(port<0,"Proposed path has no route");
            dev=DynamicCast<QbbNetDevice>(node->GetDevice(port));
        }
        uint32_t next=Peer(dev);
        NS_ABORT_MSG_IF(std::find(path.begin(),path.end(),next)!=path.end(),"Proposed route loop");
        path.push_back(next);
    }
    return path;
}
double PathDelay(const std::vector<uint32_t>& path,uint32_t bytes) {
    Time delay(0);
    for (size_t i=1;i<path.size();++i) {
        auto dev=DynamicCast<QbbNetDevice>(n.Get(path[i-1])->GetDevice(nbr2if[n.Get(path[i-1])].at(n.Get(path[i])).idx));
        auto peer=DynamicCast<QbbNetDevice>(n.Get(path[i])->GetDevice(nbr2if[n.Get(path[i])].at(n.Get(path[i-1])).idx));
        delay+=DynamicCast<QbbChannel>(dev->GetChannel())->GetDelay()+peer->GetReceiveDelay()+dev->GetDataRate().CalculateBytesTxTime(bytes);
    }
    return delay.GetSeconds();
}

class Group {
public:
uint32_t id=0, receiver=0, sourceDci=0, destinationDci=0, observer=0, pg=0, burst=0, logicalQueue=0;
double forwardDelay=0, minForwardDelay=1e100, backwardDelay=0, maxAge=0, longAge=0;
double capacity=0, sourceCapacity=0, cap=0, target=0, queueReference=0, drainTime=0;
double lastReport=0, lastPause=0, receiverPeak=0;
bool haveSnapshot=false, marking=false;
ByteHistory history;
StateHeader snapshot;
Ptr<QbbNetDevice> receiverPort, sourcePort;
struct FlowState {
    uint64_t in=0, out=0, previous=0;
    double seen=-1, lastCnp=-1, lastFeedback=-1;
    double delay=0;
    bool wan=false, active=false;
};
std::vector<FlowState> state;
std::map<std::tuple<uint32_t,uint16_t,uint16_t,uint16_t>,uint32_t> mapping;
uint64_t sourceIn=0, sourceTx=0, previousIn=0, previousTx=0;
uint64_t receiverIn=0, receiverLocal=0, receiverOut=0, lastWan=0, lastLocal=0;
uint64_t sequence=0, rejected=0, controlTx=0, controlRx=0, cnp=0;
uint64_t SourceQueue() { return sourceIn-sourceTx; }
uint64_t ReceiverQueue() { return receiverIn+receiverLocal-receiverOut; }
void Record(const char *kind,uint64_t seq,double value) {
    events << std::setprecision(15) << Simulator::Now().GetSeconds() << ',' << kind
           << ',' << id << ',' << seq << ',' << value << '\n';
}
int Match(Ptr<const Packet> packet) {
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
    packet->PeekHeader(h);
    if (h.l3Prot!=0x11 || h.dip!=serverAddress[receiver].Get()) return -1;
    auto it=mapping.find(std::make_tuple(h.sip,h.udp.sport,h.udp.dport,h.udp.pg));
    return it==mapping.end() ? -1 : int(it->second);
}
void SourceEnqueue(Ptr<const Packet> p,uint32_t q) {
    int i=Match(p);
    if (i<0 || !state[i].wan) return;
    sourceIn+=p->GetSize(); state[i].in+=p->GetSize();
    state[i].seen=Simulator::Now().GetSeconds();
}
void SourceDequeue(Ptr<const Packet> p,uint32_t) {
    int i=Match(p); if (i<0 || !state[i].wan) return;
    sourceTx+=p->GetSize(); state[i].out+=p->GetSize();
    NS_ABORT_MSG_IF(sourceTx>sourceIn || state[i].out>state[i].in,"source byte conservation");
    if (enabled) history.Add(Simulator::Now().GetNanoSeconds(),p->GetSize());
    // Nominal arrival excludes intermediate queueing; offline diagnostic only.
    Time arrival=Simulator::Now()+Seconds(state[i].delay);
    packets << std::setprecision(15) << Simulator::Now().GetSeconds() << ',' << arrival.GetSeconds()
            << ',' << id << ',' << p->GetSize() << ',' << target*8 << ',' << SourceQueue() << '\n';
}
void ReceiverEnqueue(Ptr<const Packet> p,uint32_t q) {
    if (q!=pg) return;
    int i=Match(p);
    // Same-PG reverse ACKs and other non-group traffic consume real output service.
    if (i>=0 && state[i].wan) receiverIn+=p->GetSize(); else receiverLocal+=p->GetSize();
    receiverPeak=std::max(receiverPeak,double(ReceiverQueue()));
    receiverEvents << Simulator::Now().GetNanoSeconds() << ',' << id << ',' << ReceiverQueue() << '\n';
}
void ReceiverDequeue(Ptr<const Packet> p,uint32_t q) {
    if (q!=pg) return;
    receiverOut+=p->GetSize();
    NS_ABORT_MSG_IF(receiverOut>receiverIn+receiverLocal,"receiver byte conservation");
    receiverEvents << Simulator::Now().GetNanoSeconds() << ',' << id << ',' << ReceiverQueue() << '\n';
}
void SendCnp(uint32_t i) {
    const auto &f=flows[i];
    auto packet=Create<Packet>(); CnHeader cn(f.port,f.pg,3,1,1); packet->AddHeader(cn);
    Ipv4Header ip; ip.SetSource(serverAddress[f.dst]); ip.SetDestination(serverAddress[f.src]);
    ip.SetProtocol(0xff); ip.SetTtl(64); ip.SetPayloadSize(packet->GetSize()); packet->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); packet->AddHeader(ppp);
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
    packet->PeekHeader(h);
    DynamicCast<SwitchNode>(n.Get(sourceDci))->SendNetworkControl(packet);
    ++cnp;
}
bool Receive(Ptr<const Packet> p) {
    CustomHeader ch(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
    p->PeekHeader(ch);
    if (ch.l3Prot!=249) {
        bool cn=ch.l3Prot==0xff;
        bool ack=(ch.l3Prot==0xfc || ch.l3Prot==0xfd) && (ch.ack.flags&1);
        if (cn || ack) for (uint32_t i=0;i<flows.size();++i) {
            const auto &f=flows[i];
            if (state[i].wan && ch.dip==serverAddress[f.src].Get() && ch.sip==serverAddress[f.dst].Get() &&
                (cn ? ch.cnp.fid==f.port && ch.cnp.qIndex==f.pg : ch.ack.dport==f.port && ch.ack.pg==f.pg))
                state[i].lastFeedback=Simulator::Now().GetSeconds();
        }
        return false; // never suppress native feedback
    }
    controlRx+=p->GetSize();
    auto copy=p->Copy(); PppHeader ppp; Ipv4Header ip;
    copy->RemoveHeader(ppp); copy->RemoveHeader(ip);
    if (copy->GetSize()!=StateHeader().GetSerializedSize()) { ++rejected; return true; }
    StateHeader h; copy->RemoveHeader(h);
    uint64_t now=Simulator::Now().GetNanoSeconds();
    if (h.v[StateHeader::VERSION]!=2 || h.v[StateHeader::GROUP]!=id || h.v[StateHeader::EPOCH]!=1 ||
        h.v[StateHeader::SEQ]<=snapshot.v[StateHeader::SEQ] || h.v[StateHeader::SAMPLE]>now ||
        (haveSnapshot && h.v[StateHeader::SAMPLE]<snapshot.v[StateHeader::SAMPLE]) ||
        (now-h.v[StateHeader::SAMPLE])*1e-9>maxAge) {
        ++rejected; return true;
    }
    snapshot=h; haveSnapshot=true;
    Record("STATE_RX",h.v[StateHeader::SEQ],h.v[StateHeader::QUEUE]);
    return true;
}

std::pair<double,double> Reconstruct(const StateHeader &s,int64_t end,int64_t known) {
    int64_t delay=Seconds(forwardDelay).GetNanoSeconds();
    int64_t begin=int64_t(s.v[StateHeader::SAMPLE])-delay, finish=end-delay;
    NS_ABORT_MSG_IF(!history.Covers(begin,finish,known),"invalid reconstruction interval");
    double q=s.v[StateHeader::QUEUE], peak=q;
    double net=double(s.v[StateHeader::EXTERNAL])-s.v[StateHeader::SERVICE];
    for (int64_t t=begin;t<finish;) {
        int64_t next=std::min(finish,t<0 ? int64_t(0) : (t/history.width+1)*history.width);
        q=std::max(0.0,q+history.Segment(t,next,known)+net*(next-t)*1e-9);
        peak=std::max(peak,q); t=next;
    }
    return {q,peak};
}
double Rate(double queue,double service,double external,double budget,double source,double reference,double drainTime) {
    double available=std::max(0.0,service-external);
    return std::min({budget,source,std::max(0.0,available-std::max(0.0,queue-reference)/drainTime)});
}
// Only freshness/fallback status, not a congestion or recovery state machine.
const char *Freshness(double now,bool covered) {
    if (!haveSnapshot) return now>longAge ? "FALLBACK" : "STARTUP";
    double age=now-snapshot.v[StateHeader::SAMPLE]*1e-9;
    if (age>longAge) return "FALLBACK";
    return age<=maxAge && covered ? "VALID" : "STALE";
}

void Tick() {
    double now=Simulator::Now().GetSeconds();
    int64_t nowNs=Simulator::Now().GetNanoSeconds();
    uint64_t in=sourceIn-previousIn;
    uint32_t active=0;
    double idle=std::max(2*(forwardDelay+backwardDelay),2*period);
    for (auto &f:state) {
        f.active=f.wan && f.seen>=0 && (f.in>f.out || now-f.seen<=idle || sourcePort->IsQueuePaused(pg));
        active+=f.active;
    }
    double share=SourceShare(*this,now);
    double predicted=0, peak=0, next=std::min(target,share);
    const char *status="OBSERVE";
    bool valid=false;
    int64_t action=nowNs+Seconds(forwardDelay).GetNanoSeconds();
    if (enabled) {
        history.Advance(nowNs);
        bool covered=haveSnapshot && history.Covers(int64_t(snapshot.v[StateHeader::SAMPLE])-Seconds(forwardDelay).GetNanoSeconds(),nowNs,nowNs);
        status=Freshness(now,covered); valid=std::string(status)=="VALID";
        if (predictor==2) { next=std::min(cap,share); status="STATIC"; }
        else if (valid) {
            if (predictor==0) {
                auto estimate=Reconstruct(snapshot,action,nowNs); predicted=estimate.first; peak=estimate.second;
            } else {
                predicted=snapshot.v[StateHeader::QUEUE];
                if (predictor==3) // receiver-measured constant arrival rate; no source history
                    predicted=std::max(0.0,predicted+(double(snapshot.v[StateHeader::WAN_RATE])+
                        snapshot.v[StateHeader::EXTERNAL]-snapshot.v[StateHeader::SERVICE])*
                        (action-int64_t(snapshot.v[StateHeader::SAMPLE]))*1e-9);
                peak=std::max(predicted,double(snapshot.v[StateHeader::QUEUE]));
            }
            next=Rate(predicted,snapshot.v[StateHeader::SERVICE],snapshot.v[StateHeader::EXTERNAL],
                snapshot.v[StateHeader::BUDGET],share,queueReference,drainTime);
        } else if (std::string(status)=="FALLBACK" || std::string(status)=="STARTUP") next=std::min(cap,share);
        // Short stale interval holds the last target. Long loss explicitly returns to static cap.
        if (next!=target) Record("TARGET_APPLIED_BPS",snapshot.v[StateHeader::SEQ],next*8);
        target=next;
        sourcePort->SetGroupShaperRate(logicalQueue,target);
        if (SourceQueue()>sourceHigh) marking=true;
        else if (SourceQueue()<sourceLow) marking=false;
        for (uint32_t i=0;i<state.size();++i) {
            auto &f=state[i]; double rate=(f.in-f.previous)/delta;
            if (marking && f.active && active && rate>(1+epsilon)*target/active &&
                now-f.lastCnp>=cnpInterval && now-f.lastFeedback>=cnpInterval) {
                SendCnp(i); f.lastCnp=now; Record("CNP_TX",i,rate*8);
            }
            f.previous=f.in;
        }
    }
    // Evaluation-only measurements. They never feed the source controller.
    uint64_t networkQueue=0,drops=0;
    for (uint32_t node=0;node<n.GetN();++node) if (n.Get(node)->GetNodeType()) {
        auto sw=DynamicCast<SwitchNode>(n.Get(node)); drops+=sw->m_admissionDropPackets;
        for (uint32_t d=0;d<sw->GetNDevices();++d) {
            auto dev=DynamicCast<QbbNetDevice>(sw->GetDevice(d));
            if (dev) networkQueue+=dev->GetQueue()->GetNBytesTotal();
        }
    }
    log << std::setprecision(15) << now << ',' << id << ',' << status << ',' << valid << ',' << snapshot.v[StateHeader::SEQ]
        << ',' << snapshot.v[StateHeader::SAMPLE]*1e-9 << ',' << (haveSnapshot ? now-snapshot.v[StateHeader::SAMPLE]*1e-9 : -1)
        << ',' << action*1e-9 << ',' << target*8 << ',' << in/delta*8 << ',' << (sourceTx-previousTx)/delta*8
        << ',' << SourceQueue() << ',' << sourceIn << ',' << sourceTx << ',' << snapshot.v[StateHeader::QUEUE]
        << ',' << snapshot.v[StateHeader::SERVICE]*8 << ',' << snapshot.v[StateHeader::EXTERNAL]*8
        << ',' << snapshot.v[StateHeader::BUDGET]*8 << ',' << predicted << ',' << peak << ',' << active
        << ',' << (active ? target*8/active : 0) << ',' << cnp << ',' << controlTx << ',' << controlRx << ',' << rejected
        << ',' << (SourceQueue()==0 && in/delta<0.95*target) << ',' << networkQueue << ',' << drops << ',' << share*8 << '\n';
    previousIn=sourceIn; previousTx=sourceTx;
    if (Simulator::Now()+Seconds(delta)<=Seconds(simulator_stop_time)) Simulator::Schedule(Seconds(delta),&Group::Tick,this);
}
void Report() {
    double now=Simulator::Now().GetSeconds();
    double paused=receiverPort->GetQueuePauseTime(pg).GetSeconds();
    double service=capacity*std::max(0.0,1-(paused-lastPause)/(now-lastReport));
    double external=(receiverLocal-lastLocal)/(now-lastReport);
    double wan=(receiverIn-lastWan)/(now-lastReport);
    if (receiverPort->IsQueuePaused(pg)) service=0;
    NS_ABORT_MSG_IF(ReceiverQueue()!=receiverPort->GetQueue()->GetNBytes(pg),"receiver FIFO mismatch");
    NS_ABORT_MSG_IF(enabled && SourceQueue()!=sourcePort->GetQueue()->GetLogicalBytes(logicalQueue),"source FIFO mismatch");
    receiverLog << std::setprecision(15) << now << ',' << id << ',' << ReceiverQueue() << ',' << receiverPeak << ','
        << receiverIn+receiverLocal << ',' << receiverOut << ',' << receiverIn << ',' << receiverLocal
        << ',' << service*8 << ',' << external*8 << ',' << cap*8 << ',' << paused << '\n';
    if (enabled) {
        StateHeader h;
        h.v[StateHeader::VERSION]=2; h.v[StateHeader::GROUP]=id; h.v[StateHeader::EPOCH]=1;
        h.v[StateHeader::SEQ]=++sequence; h.v[StateHeader::SAMPLE]=Simulator::Now().GetNanoSeconds();
        h.v[StateHeader::QUEUE]=ReceiverQueue(); h.v[StateHeader::SERVICE]=uint64_t(service);
        h.v[StateHeader::EXTERNAL]=uint64_t(external); h.v[StateHeader::BUDGET]=uint64_t(cap);
        h.v[StateHeader::WAN_RATE]=uint64_t(wan);
        auto p=Create<Packet>(); p->AddHeader(h);
        Ipv4Header ip; ip.SetProtocol(249); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize());
        ip.SetSource(ControlAddress(observer)); ip.SetDestination(ControlAddress(sourceDci)); p->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
        CustomHeader ch(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(ch);
        controlTx+=p->GetSize(); DynamicCast<SwitchNode>(n.Get(observer))->SendNetworkControl(p);
        Record("STATE_TX",sequence,p->GetSize());
    }
    lastReport=now; lastPause=paused; lastLocal=receiverLocal; lastWan=receiverIn; receiverPeak=ReceiverQueue();
    if (Simulator::Now()+Seconds(period)<=Seconds(simulator_stop_time)) Simulator::Schedule(Seconds(period),&Group::Report,this);
}

void Start() {
    capacity=receiverPort->GetDataRate().GetBitRate()/8.0;
    sourceCapacity=sourcePort->GetDataRate().GetBitRate()/8.0;
    cap=std::min(sourceCapacity,utilization*capacity*wanFraction); target=enabled ? cap : 0;
    queueReference=qref==0 ? capacity*40e-6 : qref;
    drainTime=tau==0 ? std::max(2*(forwardDelay+backwardDelay),4*period) : tau;
    maxAge=backwardDelay+3*period+burst/sourceCapacity;
    longAge=maxAge+3*period;
    if (enabled) history.Configure(Seconds(binWidth).GetNanoSeconds(),
        size_t(std::ceil((maxAge+forwardDelay+2*binWidth)/binWidth))+1);
    receiverPort->TraceConnectWithoutContext("QbbEnqueue",MakeCallback(&Group::ReceiverEnqueue,this));
    receiverPort->TraceConnectWithoutContext("QbbDequeue",MakeCallback(&Group::ReceiverDequeue,this));
    sourcePort->TraceConnectWithoutContext("QbbEnqueue",MakeCallback(&Group::SourceEnqueue,this));
    sourcePort->TraceConnectWithoutContext("QbbDequeue",MakeCallback(&Group::SourceDequeue,this));
    if (enabled) {
        Record("TARGET_APPLIED_BPS",0,target*8);
        sourcePort->ConfigureGroupShaper(logicalQueue,pg,target,burst);
    }
    Simulator::Schedule(Seconds(delta),&Group::Tick,this);
    Simulator::Schedule(Seconds(period),&Group::Report,this);
}
};

// Max-min allocation uses only local arrivals/backlog, independently per WAN direction.
double SourceShare(const Group &group,double now) {
    std::vector<double> demands;
    for (const auto &g:groups) {
        if (g->sourcePort!=group.sourcePort) continue;
        bool active=g.get()==&group || g->sourceIn>g->sourceTx;
        double idle=std::max(2*(g->forwardDelay+g->backwardDelay),2*period);
        for (const auto &f:g->state) active=active || (f.wan && f.seen>=0 && now-f.seen<=idle);
        if (active) demands.push_back(g->cap);
    }
    std::sort(demands.begin(),demands.end());
    double remaining=group.sourceCapacity, level=remaining;
    for (size_t i=0;i<demands.size();++i) {
        level=remaining/(demands.size()-i);
        if (demands[i]>=level) break;
        remaining-=demands[i];
    }
    return std::min(group.cap,level);
}

uint32_t Classify(uint32_t node,Ptr<const Packet> p,uint32_t priority) {
    if (priority==0) return 0;
    for (auto &g:groups) if (g->sourceDci==node && g->pg==priority) {
        int i=g->Match(p);
        if (i>=0 && g->state[i].wan) return g->logicalQueue;
    }
    return priority;
}
bool ReceiveAt(uint32_t node,Ptr<const Packet> p) {
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(h);
    if (h.l3Prot==249) {
        if (h.dip!=ControlAddress(node).Get()) return false;
        auto cp=p->Copy(); PppHeader ppp; Ipv4Header ip; cp->RemoveHeader(ppp); cp->RemoveHeader(ip);
        if (cp->GetSize()!=StateHeader().GetSerializedSize()) return true;
        StateHeader state; cp->PeekHeader(state);
        for (auto &g:groups) if (g->id==state.v[StateHeader::GROUP] && g->sourceDci==node)
            return g->Receive(p);
        return true;
    }
    for (auto &g:groups) if (g->sourceDci==node) g->Receive(p);
    return false;
}

void Setup() {
    if (output.empty()) return;
    enabled=selected_cc=="proposed";
    NS_ABORT_MSG_IF(!std::isfinite(period+delta+binWidth+qref+tau+sourceHigh+sourceLow+utilization+wanFraction+epsilon+cnpInterval) ||
        period<1e-9 || delta<1e-9 || binWidth<1e-9 || tau<0 || qref<0 || sourceLow<=0 || sourceHigh<=sourceLow ||
        sourceHigh>=buffer_size*1024.0*1024 || utilization<=0 || utilization>1 || wanFraction<=0 || wanFraction>1 ||
        epsilon<0 || cnpInterval<50e-6 || predictor>3,"invalid Proposed R2 configuration");
    // Add only dedicated control-address routes. Existing host/ECMP routes are unchanged.
    for (uint32_t target:{dci_left,dci_right}) {
        CalculateRoute(n.Get(target));
        Ipv4Address address=ControlAddress(target);
        for (uint32_t node=0;node<n.GetN();++node) if (n.Get(node)->GetNodeType() && node!=target)
            for (auto hop:nextHop[n.Get(node)][n.Get(target)])
                DynamicCast<SwitchNode>(n.Get(node))->AddTableEntry(address,nbr2if[n.Get(node)].at(hop).idx);
    }
    std::map<std::tuple<uint32_t,uint32_t,uint32_t>,Group*> byKey;
    std::vector<std::vector<uint32_t>> paths(flows.size());
    std::ofstream pathLog; RequireOutput(pathLog,output+".paths.csv","flow,group,path,nominal_forward_s");
    for (uint32_t i=0;i<flows.size();++i) {
        const auto &f=flows[i];
        auto packet=RoutePacket(serverAddress[f.src],serverAddress[f.dst],0x11);
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        h.l3Prot=0x11; h.udp.sport=f.port; h.udp.dport=f.dport;
        paths[i]=TracePath(f.src,f.dst,packet,h);
        uint32_t source=UINT32_MAX,destination=0; size_t wan=0;
        for (size_t j=1;j<paths[i].size();++j)
            if ((paths[i][j-1]==dci_left && paths[i][j]==dci_right) ||
                (paths[i][j-1]==dci_right && paths[i][j]==dci_left)) {
                source=paths[i][j-1]; destination=paths[i][j]; wan=j-1;
            }
        if (source==UINT32_MAX) continue;
        auto key=std::make_tuple(source,f.dst,f.pg);
        if (!byKey.count(key)) {
            auto g=std::make_unique<Group>(); g->id=groups.size()+1;
            g->receiver=f.dst; g->sourceDci=source; g->destinationDci=destination; g->pg=f.pg;
            NS_ABORT_MSG_IF(g->pg==0 || g->pg>=QbbNetDevice::qCnt,"invalid Proposed data PG");
            g->observer=paths[i][paths[i].size()-2];
            g->receiverPort=DynamicCast<QbbNetDevice>(n.Get(g->observer)->GetDevice(nbr2if[n.Get(g->observer)].at(n.Get(f.dst)).idx));
            g->sourcePort=source==dci_left ? dci_left_device : dci_right_device;
            g->logicalQueue=QbbNetDevice::qCnt+groups.size();
            NS_ABORT_MSG_IF(g->logicalQueue>=BEgressQueue::fCnt,"too many Proposed groups");
            CustomHeader udp(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); udp.l3Prot=0x11;
            g->burst=packet_payload_size+udp.GetSerializedSize();
            g->state.resize(flows.size());
            auto control=RoutePacket(ControlAddress(g->observer),ControlAddress(source),249);
            CustomHeader ch(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); ch.l3Prot=249;
            auto route=TracePath(g->observer,source,control,ch);
            g->backwardDelay=PathDelay(route,StateHeader().GetSerializedSize()+34);
            byKey[key]=g.get(); groups.push_back(std::move(g));
        }
        auto g=byKey.at(key);
        std::vector<uint32_t> suffix(paths[i].begin()+wan,paths[i].end()-1);
        double delay=PathDelay(suffix,g->burst);
        g->state[i].wan=true; g->state[i].delay=delay;
        g->forwardDelay=std::max(g->forwardDelay,delay); g->minForwardDelay=std::min(g->minForwardDelay,delay);
        pathLog << i << ',' << g->id << ',';
        for (size_t j=0;j<paths[i].size();++j) pathLog << (j ? "->" : "") << paths[i][j];
        pathLog << ',' << std::setprecision(15) << delay << '\n';
    }
    NS_ABORT_MSG_IF(groups.empty(),"Proposed needs at least one flow traversing the configured WAN link");
    for (auto &g:groups) for (uint32_t i=0;i<flows.size();++i) {
        auto &f=flows[i]; if (f.dst!=g->receiver || f.pg!=g->pg) continue;
        NS_ABORT_MSG_IF(paths[i][paths[i].size()-2]!=g->observer,"receiver has multiple ingress attachment points");
        g->mapping.emplace(std::make_tuple(serverAddress[f.src].Get(),f.port,f.dport,f.pg),i);
    }
    RequireOutput(log,output,"time_s,group_id,status,snapshot_valid,snapshot_seq,snapshot_time_s,report_age_s,action_time_s,target_bps,input_bps,output_bps,source_queue_bytes,source_in_bytes,source_tx_bytes,snapshot_queue_bytes,service_bps,external_bps,budget_bps,predicted_queue_bytes,predicted_peak_bytes,active_flows,fair_share_bps,cnp_sent,control_tx_bytes,control_rx_bytes,rejected_reports,sender_limited,network_queue_bytes,admission_drop_packets,source_share_bps");
    RequireOutput(receiverLog,output+".receiver.csv","time_s,group_id,queue_bytes,interval_peak_bytes,enqueued_bytes,departed_bytes,wan_bytes,local_bytes,service_bps,external_bps,budget_bps,pause_time_s");
    RequireOutput(events,output+".events.csv","time_s,event,group_id,sequence,value");
    RequireOutput(packets,output+".packets.csv","time_s,arrival_time_s,group_id,bytes,target_bps,source_queue_bytes");
    RequireOutput(receiverEvents,output+".receiver-events.csv","time_ns,group_id,queue_bytes");
    if (enabled) {
        for (uint32_t node=0;node<n.GetN();++node) if (n.Get(node)->GetNodeType())
            for (uint32_t d=0;d<n.Get(node)->GetNDevices();++d) {
                auto dev=DynamicCast<QbbNetDevice>(n.Get(node)->GetDevice(d));
                if (dev) dev->m_linkControlReceive=MakeBoundCallback(&ReceiveAt,node);
            }
    }
    for (auto &g:groups) {
        if (enabled) g->sourcePort->m_groupClassifier=MakeBoundCallback(&Classify,g->sourceDci);
        // Observation-only baselines retain their existing queue layout.
        g->Start();
    }
}
void WriteMetadata(std::ostream &out) {
    out << "  \"proposed_parameters\": {\"version\": 2, \"enabled\": " << (enabled ? "true" : "false")
        << ", \"predictor\": " << predictor << ", \"report_period_s\": " << period
        << ", \"control_period_s\": " << delta << ", \"bin_width_s\": " << binWidth
        << ", \"groups\": [";
    for (size_t i=0;i<groups.size();++i) {
        auto &g=*groups[i];
        out << (i ? "," : "") << "{\"id\":" << g.id << ",\"source_dci\":" << g.sourceDci
            << ",\"destination_dci\":" << g.destinationDci << ",\"receiver\":" << g.receiver
            << ",\"observer\":" << g.observer << ",\"pg\":" << g.pg << ",\"logical_queue\":" << g.logicalQueue
            << ",\"burst_bytes\":" << g.burst << ",\"forward_delay_s\":" << g.forwardDelay
            << ",\"min_forward_delay_s\":" << g.minForwardDelay << ",\"backward_delay_s\":" << g.backwardDelay
            << ",\"max_report_age_s\":" << g.maxAge << ",\"fallback_age_s\":" << g.longAge
            << ",\"qref_bytes\":" << g.queueReference << ",\"tau_s\":" << g.drainTime
            << ",\"history_bins\":" << g.history.bytes.size() << "}";
    }
    out << "]},\n";
}
} // namespace Proposed
