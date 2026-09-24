// Proposed-R1: one source DCI, one directly observed receiver egress and one PG.
// Included after longhaul-research.h; legacy controller remains unchanged.
#include <deque>
#include <tuple>
#include <cmath>

namespace ProposedR1 {
// All control fields are serialized, in network byte order. Rates are B/s,
// times ns and counters bytes. Protocol 249 is link-local on the DCI link.
class StateHeader : public Header {
public:
    enum Field { VERSION, KIND, GROUP, EPOCH, SEQ, START, END, QUEUE, PEAK,
                 WAN, LOCAL, DEPARTED, SERVICE, BUDGET, VALID, PAUSED, USED_SEQ,
                 TARGET, SOURCE_QUEUE, SOURCE_TX, COUNT };
    uint64_t v[COUNT] = {};
    static TypeId GetTypeId() {
        static TypeId id = TypeId("ns3::ProposedR1StateHeader").SetParent<Header>()
            .AddConstructor<StateHeader>();
        return id;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize() const override { return COUNT * 8; }
    void Serialize(Buffer::Iterator i) const override {
        for (auto x : v) i.WriteHtonU64(x);
    }
    uint32_t Deserialize(Buffer::Iterator i) override {
        for (auto &x : v) x = i.ReadNtohU64();
        return GetSerializedSize();
    }
    void Print(std::ostream &s) const override { s << "R1 kind=" << v[KIND] << " seq=" << v[SEQ]; }
};
// 0=actual history, 1=latest snapshot only, 2=static cap (same actuator/CNP).
uint32_t predictor = 0;
double period = 0.0002, delta = 0.00005;
double qref = 0, sourceHigh = 250000, emergency = 0, sourceEmergency = 0;
double tauQ = 0, tauUp = 0, cnpInterval = 0.00005;
double forwardDelay, backwardDelay, maxAge, historyWindow, historyFloor = 0;
double capacity, sourceCapacity, cap, target, predicted = 0, peak = 0;
double sourcePressureSince = -1, lastAlarm = -1, receiverPeak = 0;
double lastReport = 0, lastApply = 0, lastPause = 0, serviceEstimate = 0;
std::map<uint64_t,double> predictions;
uint32_t pg, burst;
uint64_t sourceIn = 0, sourceTx = 0, previousIn = 0, previousTx = 0;
uint64_t receiverIn = 0, receiverOut = 0, receiverLocal = 0;
uint64_t sequence = 0, applySequence = 0, acceptedApply = 0, rejected = 0;
uint64_t controlTx = 0, controlRx = 0, cnp = 0;
StateHeader snapshot;
bool haveSnapshot = false;
std::string mode = "INIT";
Ptr<QbbNetDevice> receiverPort;
std::ofstream log, events, packets, receiverLog;
struct FlowState {
    uint64_t in = 0, out = 0, previous = 0;
    double seen = -1, lastCnp = -1, lastFeedback = -1;
    bool active = false;
};
std::vector<FlowState> state;
std::map<std::tuple<uint32_t,uint16_t,uint16_t,uint16_t>, uint32_t> mapping;
struct Sent { double arrival; uint32_t bytes; };
std::deque<Sent> history;

int Match(Ptr<const Packet> packet) {
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(h);
    if (h.l3Prot != 0x11 || h.dip != serverAddress[research_receiver].Get()) return -1;
    auto it = mapping.find(std::make_tuple(h.sip,h.udp.sport,h.udp.dport,h.udp.pg));
    return it == mapping.end() ? -1 : int(it->second);
}
uint64_t SourceQueue() { return sourceIn - sourceTx; }
uint64_t ReceiverQueue() { return receiverIn + receiverLocal - receiverOut; }
void Record(const char *kind, uint64_t seq, double value) {
    events << std::setprecision(12) << Simulator::Now().GetSeconds() << ',' << kind
           << ',' << seq << ',' << value << '\n';
}
void SendState(uint64_t kind);
void SourceEnqueue(Ptr<const Packet> p, uint32_t q) {
    int i = Match(p);
    NS_ABORT_MSG_IF(q == pg && i < 0, "R1 source PG contains an unregistered group");
    if (i < 0) return;
    sourceIn += p->GetSize(); state[i].in += p->GetSize();
    state[i].seen = Simulator::Now().GetSeconds(); state[i].active = true;
}
void SourceDequeue(Ptr<const Packet> p, uint32_t) {
    int i = Match(p); if (i < 0) return;
    double now = Simulator::Now().GetSeconds();
    sourceTx += p->GetSize(); state[i].out += p->GetSize();
    NS_ABORT_MSG_IF(sourceTx > sourceIn || state[i].out > state[i].in, "R1 source conservation");
    // QbbChannel delivers after serialization, propagation and receiver processing.
    double arrival = now + forwardDelay + p->GetSize()/sourceCapacity;
    history.push_back({arrival, p->GetSize()});
    packets << std::setprecision(12) << now << ',' << arrival << ',' << p->GetSize()
            << ',' << target*8 << ',' << SourceQueue() << '\n';
}
void ReceiverEnqueue(Ptr<const Packet> p, uint32_t q) {
    if (q != pg) return;
    NS_ABORT_MSG_IF(Match(p)<0,"R1 first version excludes local competing traffic on the controlled PG");
    receiverIn += p->GetSize();
    receiverPeak = std::max(receiverPeak, double(ReceiverQueue()));
    double now = Simulator::Now().GetSeconds();
    if (ReceiverQueue() >= emergency && now-lastAlarm >= delta) {
        lastAlarm = now;
        Simulator::ScheduleNow(&SendState, uint64_t(3));
    }
}
void ReceiverDequeue(Ptr<const Packet> p, uint32_t q) {
    if (q != pg) return;
    receiverOut += p->GetSize();
    NS_ABORT_MSG_IF(receiverOut > receiverIn + receiverLocal, "R1 receiver conservation");
}

void SendState(uint64_t kind) {
    double now = Simulator::Now().GetSeconds();
    StateHeader h;
    h.v[StateHeader::VERSION]=1; h.v[StateHeader::KIND]=kind;
    h.v[StateHeader::GROUP]=1; h.v[StateHeader::EPOCH]=1;
    h.v[StateHeader::SEQ]=kind==2 ? ++applySequence : ++sequence;
    h.v[StateHeader::START]=uint64_t(lastReport*1e9);
    h.v[StateHeader::END]=Simulator::Now().GetNanoSeconds();
    h.v[StateHeader::VALID]=uint64_t(maxAge*1e9);
    if (kind != 2) {
        // Empty queues retain line capacity. A known pause immediately withdraws service.
        bool paused = receiverPort->IsQueuePaused(pg);
        h.v[StateHeader::QUEUE]=ReceiverQueue(); h.v[StateHeader::PEAK]=receiverPeak;
        h.v[StateHeader::WAN]=receiverIn; h.v[StateHeader::LOCAL]=receiverLocal;
        h.v[StateHeader::DEPARTED]=receiverOut;
        h.v[StateHeader::PAUSED]=paused;
        h.v[StateHeader::SERVICE]=paused ? 0 : uint64_t(serviceEstimate);
        h.v[StateHeader::BUDGET]=paused ? 0 : uint64_t(std::min(cap,research_target_util*serviceEstimate));
    } else {
        h.v[StateHeader::USED_SEQ]=snapshot.v[StateHeader::SEQ];
        h.v[StateHeader::TARGET]=uint64_t(target);
        h.v[StateHeader::SOURCE_QUEUE]=SourceQueue();
        h.v[StateHeader::SOURCE_TX]=sourceTx;
    }
    auto p = Create<Packet>(); p->AddHeader(h);
    Ipv4Header ip; ip.SetProtocol(249); ip.SetTtl(1); ip.SetPayloadSize(p->GetSize());
    // Consumed only by the configured DCI peer; no IP routing is involved.
    ip.SetSource(Ipv4Address("192.0.2.1")); ip.SetDestination(Ipv4Address("192.0.2.2"));
    p->AddHeader(ip); PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    p->PeekHeader(ch);
    controlTx += p->GetSize();
    (kind==2 ? dci_left_device : dci_right_device)->SwitchSend(0,p,ch);
    Record(kind==1 ? "GROUP_STATE_TX" : kind==2 ? "APPLY_STATUS_TX" : "ALARM_TX",
           h.v[StateHeader::SEQ],p->GetSize());
}

bool Receive(bool source, Ptr<const Packet> p) {
    auto copy = p->Copy(); PppHeader ppp; Ipv4Header ip;
    copy->RemoveHeader(ppp); copy->RemoveHeader(ip);
    if (ip.GetProtocol()!=249) {
        // Standard feedback is always forwarded. Observe both CNP and CE-bearing ACK.
        if (source) {
            CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
            p->PeekHeader(ch);
            bool cn = ch.l3Prot==0xff;
            bool ack = (ch.l3Prot==0xfc || ch.l3Prot==0xfd) && (ch.ack.flags & 1);
            if (cn || ack) for (uint32_t i=0; i<flows.size(); ++i) {
                const auto &f=flows[i];
                if (ch.dip==serverAddress[f.src].Get() && ch.sip==serverAddress[f.dst].Get() &&
                    (cn ? ch.cnp.fid==f.port && ch.cnp.qIndex==f.pg :
                          ch.ack.dport==f.port && ch.ack.pg==f.pg))
                    state[i].lastFeedback=Simulator::Now().GetSeconds();
            }
        }
        return false;
    }
    if (copy->GetSize()!=StateHeader().GetSerializedSize()) return false;
    StateHeader h; copy->RemoveHeader(h); controlRx += p->GetSize();
    if (h.v[StateHeader::VERSION]!=1 || h.v[StateHeader::GROUP]!=1 ||
        h.v[StateHeader::EPOCH]!=1 || h.v[StateHeader::END]>uint64_t(Simulator::Now().GetNanoSeconds())) {
        ++rejected; return true;
    }
    if (!source) {
        if (h.v[StateHeader::KIND]!=2 || h.v[StateHeader::SEQ]<=acceptedApply) ++rejected;
        else { acceptedApply=h.v[StateHeader::SEQ]; Record("APPLY_STATUS_RX",acceptedApply,h.v[StateHeader::TARGET]*8.0); }
        return true;
    }
    if ((h.v[StateHeader::KIND]!=1 && h.v[StateHeader::KIND]!=3) ||
        h.v[StateHeader::SEQ]<=snapshot.v[StateHeader::SEQ]) { ++rejected; return true; }
    auto prediction=predictions.find(h.v[StateHeader::END]);
    if (prediction!=predictions.end())
        Record("PREDICTION_ERROR_BYTES",h.v[StateHeader::SEQ],double(h.v[StateHeader::QUEUE])-prediction->second);
    predictions.erase(predictions.begin(),predictions.upper_bound(h.v[StateHeader::END]));
    snapshot=h; haveSnapshot=true;
    Record("STATE_RX",h.v[StateHeader::SEQ],h.v[StateHeader::QUEUE]);
    return true;
}

// Replay packets, not average-rate extrapolation. Idle service is discarded at
// every event. This also retains intermediate peaks without binning artifacts.
std::pair<double,double> Reconstruct(double end) {
    double t=snapshot.v[StateHeader::END]*1e-9;
    double q=snapshot.v[StateHeader::QUEUE], high=q;
    double service=snapshot.v[StateHeader::SERVICE];
    for (const auto &e:history) {
        if (e.arrival<=t || e.arrival>end) continue;
        q=std::max(0.0,q-service*(e.arrival-t))+e.bytes;
        high=std::max(high,q); t=e.arrival;
    }
    q=std::max(0.0,q-service*(end-t));
    return {q,high};
}

void Tick() {
    double now=Simulator::Now().GetSeconds();
    while (!history.empty() && history.front().arrival < now-historyWindow) {
        historyFloor=history.front().arrival; history.pop_front();
    }
    uint64_t in=sourceIn-previousIn;
    uint32_t active=0;
    double idle=std::max(2*(forwardDelay+backwardDelay),4*period);
    for (auto &f:state) {
        f.active=f.seen>=0 && (f.in>f.out || now-f.seen<=idle || dci_left_device->IsQueuePaused(pg));
        active+=f.active;
    }
    double age=haveSnapshot ? now-snapshot.v[StateHeader::END]*1e-9 : -1;
    bool valid=haveSnapshot && age<=maxAge &&
        snapshot.v[StateHeader::END]*1e-9>=historyFloor;
    double next=target;
    if (!haveSnapshot) mode="INIT";
    else if (!valid) { mode="STALE"; next=std::min(target,cap); }
    else {
        auto estimate=Reconstruct(now+forwardDelay+burst/sourceCapacity);
        predicted=estimate.first; peak=estimate.second;
        double q=predictor==1 ? double(snapshot.v[StateHeader::QUEUE]) : predicted;
        double upper=std::min({cap,double(snapshot.v[StateHeader::BUDGET]),double(snapshot.v[StateHeader::SERVICE])});
        double raw=std::max(0.0,upper-std::max(0.0,q-qref-std::max(double(burst),0.1*qref))/tauQ);
        if (predictor==2) raw=cap;
        next=std::min(raw,target+upper*delta/tauUp);
        mode=q>qref+std::max(double(burst),0.1*qref) ? "DRAIN" : next<upper ? "RECOVER" : "TRACK";
        if (predictor!=2 && (snapshot.v[StateHeader::QUEUE]>=emergency ||
                            peak>=emergency || snapshot.v[StateHeader::PAUSED])) {
            next=0; mode="EMERGENCY";
        }
    }
    if (valid && predictor==0) {
        // Publish once per future report grid time; pair only on exact sample time.
        int64_t step=Seconds(period).GetNanoSeconds();
        int64_t end=int64_t((now+forwardDelay)*1e9)/step*step;
        if (end>Simulator::Now().GetNanoSeconds() && !predictions.count(end)) {
            predictions[end]=Reconstruct(end*1e-9).first;
            Record("PREDICTION_PUBLISHED",end,predictions[end]);
        }
    }
    if (next!=target) Record("TARGET_APPLIED_BPS",snapshot.v[StateHeader::SEQ],next*8);
    target=next;
    // This update may immediately dequeue a packet; record the applied target first.
    dci_left_device->SetQueueShaperRate(target);
    double qs=SourceQueue();
    if (qs>sourceHigh) { if (sourcePressureSince<0) sourcePressureSince=now; }
    else if (qs<sourceHigh/4) sourcePressureSince=-1;
    bool pressure=qs>=sourceEmergency || (qs>sourceHigh && sourcePressureSince>=0 &&
        now-sourcePressureSince>=std::max(2*delta,0.0001) && in/delta>target);
    for (uint32_t i=0;i<state.size();++i) {
        auto &f=state[i]; double rate=(f.in-f.previous)/delta;
        if (pressure && f.active && active && rate>(1+research_deadband)*target/active &&
            now-f.lastCnp>=cnpInterval && now-f.lastFeedback>=cnpInterval) {
            ResearchCnp(i); ++cnp; f.lastCnp=now; Record("CNP_TX",i,rate*8);
        }
        f.previous=f.in;
    }
    if (now-lastApply>=period-1e-12) { SendState(2); lastApply=now; }
    // Evaluation only: these global counters never enter the online controller.
    uint64_t networkQueue=0, drops=0;
    for (uint32_t node=0;node<n.GetN();++node) if (n.Get(node)->GetNodeType()) {
        auto sw=DynamicCast<SwitchNode>(n.Get(node)); drops+=sw->m_admissionDropPackets;
        for (uint32_t d=0;d<sw->GetNDevices();++d) {
            auto dev=DynamicCast<QbbNetDevice>(sw->GetDevice(d));
            if (dev) networkQueue+=dev->GetQueue()->GetNBytesTotal();
        }
    }
    log << std::setprecision(12) << now << ',' << mode << ',' << valid << ','
        << snapshot.v[StateHeader::SEQ] << ',' << age << ',' << target*8 << ','
        << in/delta*8 << ',' << (sourceTx-previousTx)/delta*8 << ',' << qs << ',' << sourceIn << ',' << sourceTx << ','
        << snapshot.v[StateHeader::QUEUE] << ',' << predicted << ',' << peak << ',' << active << ','
        << (active ? target*8/active : 0) << ',' << cnp << ',' << controlTx << ',' << controlRx << ','
        << rejected << ',' << (qs==0 && in/delta<0.95*target) << ',' << networkQueue << ',' << drops << '\n';
    previousIn=sourceIn; previousTx=sourceTx;
    if (now+delta<=simulator_stop_time) Simulator::Schedule(Seconds(delta),&Tick);
}
void PauseEvent(uint32_t) {
    // The trace is emitted before the device updates its pause flag.
    Simulator::ScheduleNow(&SendState,uint64_t(3));
}
void Report() {
    double now=Simulator::Now().GetSeconds();
    double paused=receiverPort->GetQueuePauseTime(pg).GetSeconds();
    serviceEstimate=capacity*std::max(0.0,1-(paused-lastPause)/(now-lastReport));
    lastPause=paused;
    // Validate measurement against the real FIFO; no queue is allocated outside MMU.
    NS_ABORT_MSG_IF(ReceiverQueue()!=receiverPort->GetQueue()->GetNBytes(pg),"R1 receiver FIFO mismatch");
    NS_ABORT_MSG_IF(SourceQueue()!=dci_left_device->GetQueue()->GetNBytes(pg),"R1 source FIFO mismatch");
    receiverLog << std::setprecision(12) << now << ',' << ReceiverQueue() << ',' << receiverPeak << ','
        << receiverIn << ',' << receiverOut << ',' << serviceEstimate*8 << ',' << paused << '\n';
    SendState(1); lastReport=Simulator::Now().GetSeconds(); receiverPeak=ReceiverQueue();
    Record("RECEIVER_QUEUE",sequence,ReceiverQueue());
    if (lastReport+period<=simulator_stop_time) Simulator::Schedule(Seconds(period),&Report);
}

void Setup() {
    NS_ABORT_MSG_IF(flows.empty() || research_receiver>=n.GetN(),"R1 needs registered flows and receiver");
    auto it=nbr2if[n.Get(dci_right)].find(n.Get(research_receiver));
    NS_ABORT_MSG_IF(it==nbr2if[n.Get(dci_right)].end(),"R1 receiver must directly attach to receiver DCI");
    receiverPort=DynamicCast<QbbNetDevice>(n.Get(dci_right)->GetDevice(it->second.idx));
    pg=flows.front().pg;
    NS_ABORT_MSG_IF(pg==0 || pg>=QbbNetDevice::qCnt,"R1 requires a data priority");
    state.resize(flows.size());
    for (uint32_t i=0;i<flows.size();++i) {
        auto &f=flows[i];
        NS_ABORT_MSG_IF(f.dst!=research_receiver || f.pg!=pg ||
            nbr2if[n.Get(dci_left)].count(n.Get(f.src))==0,
            "R1 supports one source DCI, directly attached senders, one receiver and one PG");
        auto key=std::make_tuple(serverAddress[f.src].Get(),f.port,f.dport,f.pg);
        NS_ABORT_MSG_IF(!mapping.emplace(key,i).second,"duplicate R1 QP registration");
    }
    capacity=receiverPort->GetDataRate().GetBitRate()/8.0;
    sourceCapacity=dci_left_device->GetDataRate().GetBitRate()/8.0;
    cap=std::min(sourceCapacity,research_target_util*capacity); target=cap;
    // Actual source-start -> remote enqueue delay additionally includes each frame's serialization.
    forwardDelay=DynamicCast<QbbChannel>(dci_left_device->GetChannel())->GetDelay().GetSeconds()+dci_right_device->GetReceiveDelay().GetSeconds();
    backwardDelay=DynamicCast<QbbChannel>(dci_right_device->GetChannel())->GetDelay().GetSeconds()+dci_left_device->GetReceiveDelay().GetSeconds();
    // Use the exact UDP/INT header size employed by RdmaHw, not a guessed Ethernet MTU.
    CustomHeader udp(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
    udp.l3Prot=0x11; burst=packet_payload_size+udp.GetSerializedSize();
    if (qref==0) qref=capacity*40e-6;
    if (tauQ==0) tauQ=std::max(2*(forwardDelay+backwardDelay),4*period);
    if (tauUp==0) tauUp=std::max(forwardDelay+backwardDelay,4*delta);
    // Conservative explicit emergency reserve inside the configured shared pool.
    if (emergency==0) emergency=buffer_size*1024.0*1024/4;
    if (sourceEmergency==0) sourceEmergency=buffer_size*1024.0*1024/4;
    serviceEstimate=capacity;
    maxAge=backwardDelay+3*period+burst/sourceCapacity;
    historyWindow=maxAge+forwardDelay+2*delta+burst/sourceCapacity;
    NS_ABORT_MSG_IF(!std::isfinite(period+delta+qref+sourceHigh+emergency+sourceEmergency+tauQ+tauUp+cnpInterval) ||
        period<=0 || delta<=0 || qref<=0 || sourceHigh<=0 || emergency<=qref || sourceEmergency<=sourceHigh || sourceEmergency>=buffer_size*1024.0*1024 ||
        emergency>=buffer_size*1024.0*1024 || tauQ<=0 || tauUp<=0 || cnpInterval<50e-6 ||
        research_target_util<=0 || research_target_util>1 || predictor>2,"invalid R1 configuration");
    RequireOutput(log,research_output,"time_s,mode,target_valid,snapshot_seq,report_age_s,target_bps,input_bps,output_bps,source_queue_bytes,source_in_bytes,source_tx_bytes,snapshot_queue_bytes,predicted_queue_bytes,predicted_peak_bytes,active_flows,fair_share_bps,cnp_sent,control_tx_bytes,control_rx_bytes,rejected_reports,sender_limited,network_queue_bytes,admission_drop_packets");
    RequireOutput(receiverLog,research_output+".receiver.csv","time_s,queue_bytes,interval_peak_bytes,enqueued_bytes,departed_bytes,service_bps,pause_time_s");
    RequireOutput(events,research_output+".events.csv","time_s,event,sequence,value");
    RequireOutput(packets,research_output+".packets.csv","time_s,arrival_time_s,bytes,target_bps,source_queue_bytes");
    receiverPort->TraceConnectWithoutContext("QbbEnqueue",MakeCallback(&ReceiverEnqueue));
    receiverPort->TraceConnectWithoutContext("QbbDequeue",MakeCallback(&ReceiverDequeue));
    dci_left_device->TraceConnectWithoutContext("QbbEnqueue",MakeCallback(&SourceEnqueue));
    dci_left_device->TraceConnectWithoutContext("QbbDequeue",MakeCallback(&SourceDequeue));
    dci_left_device->m_linkControlReceive=MakeBoundCallback(&Receive,true);
    dci_right_device->m_linkControlReceive=MakeBoundCallback(&Receive,false);
    receiverPort->TraceConnectWithoutContext("QbbPfc",MakeCallback(&PauseEvent));
    Record("TARGET_APPLIED_BPS",0,target*8);
    dci_left_device->ConfigureQueueShaper(pg,target,burst);
    Simulator::Schedule(Seconds(delta),&Tick);
    Simulator::Schedule(Seconds(period),&Report);
}
} // namespace ProposedR1
