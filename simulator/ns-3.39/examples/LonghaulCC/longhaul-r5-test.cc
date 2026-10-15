#include "ns3/dci-gateway-node.h"
#include "ns3/rdma-hw.h"
#include "ns3/qbb-header.h"
#include "ns3/ppp-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/pause-header.h"
#include "ns3/cn-header.h"
#include "ns3/simulator.h"
#include "ns3/interface-tag.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace ns3 {
class TestNode : public Node {
public:
    std::vector<CustomHeader> received;
    bool SwitchReceiveFromDevice(Ptr<NetDevice>,Ptr<Packet>,CustomHeader& h) override { received.push_back(h); return true; }
    void SwitchNotifyDequeue(uint32_t,uint32_t,Ptr<Packet>) override {}
};
class TestDevice : public QbbNetDevice {
public:
    void Pause(bool value) {
        if (value && !m_paused[3]) m_pauseStarted[3]=Simulator::Now();
        if (!value && m_paused[3]) m_pauseTotal[3]+=Simulator::Now()-m_pauseStarted[3];
        m_paused[3]=value;
    }
};
class DciGatewayTestCase {
    using G=DciGatewayNode;
    static void Check(bool v,const char* why) { if (!v) throw std::runtime_error(why); }
    static bool Near(double a,double b) { return std::abs(a-b)<1e-6; }
    static CustomHeader Header(Ptr<const Packet> p) {
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(h); return h;
    }
    static Ptr<Packet> Data(uint16_t sport=10001,uint8_t ecn=0) {
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        h.pppProto=0x0021; h.l3Prot=0x11; h.sip=1; h.dip=2; h.m_tos=ecn;
        h.udp.pg=3; h.udp.sport=sport; h.udp.dport=20001;
        auto p=Create<Packet>(1048-h.GetSerializedSize()); p->AddHeader(h); return p;
    }
    static Ptr<Packet> PausePacket(uint32_t us) {
        auto p=Create<Packet>(); p->AddHeader(PauseHeader(us,0,3));
        Ipv4Header ip; ip.SetProtocol(0xfe); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); return p;
    }
    static Ptr<TestDevice> Device(Ptr<Node> n,Ptr<QbbChannel> c) {
        auto d=CreateObject<TestDevice>(); d->SetDataRate(DataRate("100Gbps"));
        d->SetQueue(CreateObject<BEgressQueue>()); n->AddDevice(d); d->Attach(c); return d;
    }
    static Ptr<G> Gateway(bool source) {
        auto g=CreateObject<G>(); g->m_enabled=true; g->SetNodeType(2);
        auto c=CreateObject<QbbChannel>(); Device(g,c);
        auto peer=CreateObject<TestNode>(); peer->SetNodeType(2); Device(peer,c);
        for (uint32_t ip:{1u,2u,G::ControlAddress(999).Get()}) { Ipv4Address dst(ip); g->AddTableEntry(dst,0); }
        G::Registration r; r.id=1; r.peer=999; r.sip=1; r.dip=2; r.sport=10001; r.dport=20001;
        r.role=source ? G::Role::Source : G::Role::Receiver;
        r.propagation=MilliSeconds(5); r.localResponse=MicroSeconds(100); g->RegisterFlow(r); return g;
    }
    static G::Record State(uint32_t id,uint64_t seq,uint64_t sample) {
        G::Record r{}; r[G::GROUP]=id; r[G::SEQ]=seq; r[G::SAMPLE]=sample;
        r[G::SERVICE]=1000000; r[G::CAPACITY]=12500000000; r[G::BUFFER]=1000000; r[G::FLAGS]=G::VALID; return r;
    }
    static Ptr<Packet> Wire(const G::Record& r,uint32_t version=5) {
        std::vector<uint8_t> bytes;
        auto word=[&](uint64_t v,unsigned n) { for (int i=n-1;i>=0;--i) bytes.push_back((v>>(8*i))&255); };
        word(version,4); word(17,4); for (auto v:r) word(v,8);
        auto p=Create<Packet>(bytes.data(),bytes.size()); Ipv4Header ip; ip.SetSource(G::ControlAddress(999));
        ip.SetProtocol(249); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); return p;
    }
    static void Replay() {
        G::History h; h.Add(1000000,20000); auto p=h.Replay(0,3000000,500000,0,10000000);
        Check(Near(p.queue,0) && Near(p.peak,20000),"drained endpoint hid intermediate peak");
        Check(Near(h.Replay(0,1000000,0,0,10000000).queue,20000),"idle service cancelled future arrival");
        Check(Near(h.Replay(1000000,1000000,1000000,0,10000000,0).queue,20000),"same-ns later arrival omitted");
        Check(Near(h.Replay(1000000,1000000,1000000,20000,10000000,20000).queue,20000),"same-ns earlier arrival counted twice");
        Check(Near(h.Replay(1000000,2000000,1000000,20000,-1000000).queue,21000),"b > c accounting");
        h.Prune(1500000); Check(!h.Covers(1000000) && h.Covers(1500000),"history coverage");
    }
    static void Controller() {
        auto a=Gateway(true); auto id=a->m_groups.begin()->first;
        auto p=Wire(State(id,1,0)); a->ReceiveReport(p,Header(p)); Check(a->m_groups.at(id).have,"wire state rejected");
        a->ReceiveReport(p,Header(p)); Check(a->m_rejected==1,"duplicate state accepted");
        p=Wire(State(id,2,0),4); a->ReceiveReport(p,Header(p)); Check(a->m_rejected==2,"R4 protocol accepted");
        auto& g=a->m_groups.at(id); g.in=1048; a->UpdateSource(g); Check(Near(g.candidate,26000000),"proportional control units/formula mismatch");
        Simulator::Schedule(NanoSeconds(1),[a,id]() {
            auto& g=a->m_groups.at(id); auto r=State(id,2,1); r[G::SERVICE]=0; r[G::FLAGS]|=G::PAUSED;
            Check(a->ApplyRecord(g,r),"pause rejected"); a->UpdateSource(g); Check(!g.candidate,"pause topped up Q target");
        });
        Simulator::Schedule(MilliSeconds(1),[a,id]() {
            auto& g=a->m_groups.at(id); auto r=State(id,3,1000000); a->ApplyRecord(g,r);
            a->UpdateSource(g); Check(g.candidate>0,"resume failed");
            g.state[G::BUFFER]=10000; g.state[G::SERVICE]=100000000; g.history.Add(2000000,20000);
            g.target=100000000; a->m_config.margin=1000; a->UpdateSource(g);
            Check(g.predicted.peak==20000 && g.predicted.queue==0 && !g.candidate,"peak protection failed");
        });
        Simulator::Schedule(MilliSeconds(25),[a,id]() {
            auto& g=a->m_groups.at(id); a->UpdateSource(g); Check(!g.candidate && !g.fresh,"stale state sends");
            a->ApplyRecord(g,State(id,4,25000000)); a->UpdateSource(g); Check(g.candidate>0,"fresh feedback cannot recover");
        });
        Simulator::Stop(MilliSeconds(26)); Simulator::Run(); Simulator::Destroy();
    }
    static void Service() {
        auto b=Gateway(false); auto id=b->m_groups.begin()->first;
        Simulator::Schedule(MicroSeconds(200),[b,id]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g); Check(g.service==12.5e9,"low supply reduced exclusive capacity");
            g.shared=true; g.emptied=true;
        });
        Simulator::Schedule(MicroSeconds(400),[b,id]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g); Check(g.service==12.5e9,"empty shared queue lost estimate");
            g.in=1000100; g.tx=1000000; g.emptied=false; g.shared=true;
        });
        Simulator::Schedule(MicroSeconds(600),[b,id]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g); Check(Near(g.service,5e9),"busy interval service estimate");
            DynamicCast<TestDevice>(b->Port(0))->Pause(true);
        });
        Simulator::Schedule(MicroSeconds(800),[b,id]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g); Check(!g.state[G::SERVICE],"pause ignored");
            DynamicCast<TestDevice>(b->Port(0))->Pause(false); g.emptied=true;
        });
        Simulator::Schedule(MicroSeconds(1200),[b,id]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g); Check(g.state[G::SERVICE]>0,"service stuck after resume");
        });
        Simulator::Stop(MilliSeconds(2)); Simulator::Run(); Simulator::Destroy();
    }
    static void Backpressure() {
        auto a=Gateway(true); auto& g=a->m_groups.begin()->second; auto& f=a->m_flows.at(1);
        f.in=2000; f.lastCnp=1000000;
        Check(!a->NeedCnp(g,f,false,true,false,1100000),"no response wait");
        Check(a->NeedCnp(g,f,true,true,false,1100000),"emergency cannot bypass wait");
        Check(!a->NeedCnp(g,f,true,true,false,1020000),"emergency violated CNP minimum interval");
        Check(!a->NeedCnp(g,f,false,true,true,2000000),"draining still sends ordinary CNP");
        Check(a->NeedCnp(g,f,false,true,false,2000000),"renewed growth cannot trigger");
        f.lastNativeCnp=1900000;
        Check(!a->NeedCnp(g,f,false,true,false,2000000),"native CNP response was not awaited");
        Check(a->NeedCnp(g,f,true,true,false,2000000),"native CNP blocked emergency protection");
        f.previousIn=f.in; Check(!a->NeedCnp(g,f,true,true,false,2000000),"idle QP targeted"); Simulator::Destroy();
    }
    static void PauseAvailability() {
        auto b=Gateway(false); auto id=b->m_groups.begin()->first;
        auto d=DynamicCast<TestDevice>(b->Port(0));
        Simulator::Schedule(MicroSeconds(200),[b,id]() { b->SampleReceiver(b->m_groups.at(id)); });
        Simulator::Schedule(MicroSeconds(250),[d]() { d->Pause(true); });
        Simulator::Schedule(MicroSeconds(350),[d]() { d->Pause(false); });
        Simulator::Schedule(MicroSeconds(400),[b,id]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g);
            Check(g.state[G::PAUSE_NS]==100000 && Near(g.availability,.5),"partial pause accounting");
            Check(g.service==12.5e9 && g.state[G::SERVICE]==6250000000,
                  "exclusive service did not account for pause availability");
            g.shared=true; g.emptied=false; g.in=500100; g.tx=500000;
        });
        Simulator::Schedule(MicroSeconds(450),[d]() { d->Pause(true); });
        Simulator::Schedule(MicroSeconds(550),[d]() { d->Pause(false); });
        Simulator::Schedule(MicroSeconds(600),[b,id]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g);
            Check(Near(g.service,5e9) && g.state[G::SERVICE]==2500000000,
                  "shared busy service failed to separate active time and availability");
            g.tx=g.in; g.shared=true; g.emptied=true;
        });
        Simulator::Schedule(MicroSeconds(650),[d]() { d->Pause(true); });
        Simulator::Schedule(MicroSeconds(750),[d]() { d->Pause(false); });
        Simulator::Schedule(MicroSeconds(800),[b,id,d]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g);
            Check(Near(g.service,5e9) && g.state[G::SERVICE]==2500000000,
                  "idle shared interval lost capacity or ignored pause");
            g.shared=true; d->Pause(true);
        });
        Simulator::Schedule(MicroSeconds(1000),[b,id,d]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g);
            Check(!g.state[G::SERVICE] && (g.state[G::FLAGS]&G::PAUSED),"full pause not explicit");
            g.shared=true; d->Pause(false);
        });
        Simulator::Schedule(MicroSeconds(1200),[b,id]() {
            auto& g=b->m_groups.at(id); b->SampleReceiver(g);
            Check(g.state[G::SERVICE]==5000000000 && g.availability==1,"pause poisoned retained service");
        });
        Simulator::Stop(MicroSeconds(1201)); Simulator::Run(); Simulator::Destroy();
    }
    static uint32_t Queue(Ptr<const Packet> p,uint32_t) { return Header(p).udp.sport==10001 ? 8 : 9; }
    static void SharedShaper() {
        auto n=CreateObject<TestNode>(); n->SetNodeType(2); auto peer=CreateObject<TestNode>(); peer->SetNodeType(2);
        auto c=CreateObject<QbbChannel>(); auto tx=Device(n,c); Device(peer,c);
        tx->ConfigureInjectionGroup(1,1048); tx->BindInjectionQueue(8,3,1); tx->BindInjectionQueue(9,3,1);
        tx->m_groupClassifier=MakeCallback(&DciGatewayTestCase::Queue); tx->ApplyInjectionControls({{1,1048000}});
        for (unsigned i=0;i<8;++i) { auto p=Data(); auto h=Header(p); tx->SwitchSend(3,p,h); }
        Simulator::Schedule(MilliSeconds(3),[tx]() { auto p=Data(10002); auto h=Header(p); tx->SwitchSend(3,p,h); });
        Simulator::Schedule(MicroSeconds(4500),[tx,peer]() {
            Check(peer->received.size()<=4,"startup budget multiplied by flows");
            bool second=false; for (auto h:peer->received) second|=h.udp.sport==10002;
            Check(second,"backlogged flow starved"); tx->ApplyInjectionControls({{1,0}});
        });
        Simulator::Schedule(MicroSeconds(6500),[tx,peer]() {
            Check(peer->received.size()<=4,"zero rate still sent packets"); tx->ApplyInjectionControls({{1,1048000}});
        });
        Simulator::Stop(MilliSeconds(14)); Simulator::Run(); Check(peer->received.size()==9,"idle share not reused or resume failed"); Simulator::Destroy();
    }
    static uint32_t InterleavedQueue(Ptr<const Packet> p,uint32_t) { return 8+Header(p).udp.sport-10001; }
    static void InterleavedGroups() {
        auto n=CreateObject<TestNode>(),peer=CreateObject<TestNode>(); n->SetNodeType(2); peer->SetNodeType(2);
        auto c=CreateObject<QbbChannel>(); auto tx=Device(n,c); Device(peer,c);
        tx->SetDataRate(DataRate("400Gbps"));
        tx->ConfigureInjectionGroup(1,1048); tx->ConfigureInjectionGroup(2,1048);
        tx->BindInjectionQueue(8,3,1); tx->BindInjectionQueue(9,3,2); tx->BindInjectionQueue(10,3,1);
        tx->m_groupClassifier=MakeCallback(&DciGatewayTestCase::InterleavedQueue);
        tx->ApplyInjectionControls({{1,12.5e9},{2,12.5e9}});
        for (unsigned i=0;i<2000;++i) for (uint16_t sport:{10001,10002,10003}) {
            auto p=Data(sport); auto h=Header(p); tx->SwitchSend(3,p,h);
        }
        Simulator::Stop(MicroSeconds(100)); Simulator::Run();
        std::map<uint16_t,int> count;
        for (const auto& h:peer->received) ++count[h.udp.sport];
        Check(count[10001]>100 && std::abs(count[10001]-count[10003])<=1,
              "another group advanced the shared group's round robin position");
        Check(count[10002]>100,"group rotation blocked an independent group");
        Check((count[10001]+count[10003])*1048<=1250000 && count[10002]*1048<=1250000,
              "group rotation exceeded shared token budgets");
        Simulator::Destroy();
    }
    static void IdleActivation() {
        auto a=Gateway(true);
        auto r=a->m_flows.at(1).reg; r.id=2; r.sport=10002; r.remotePort=1;
        a->RegisterFlow(r);
        a->m_mmu->SetBufferPool(10000000); a->m_mmu->SetIngressPool(10000000);
        a->m_mmu->SetEgressLosslessPool(10000000); a->m_mmu->reserveIngress[0][3]=10000000;
        auto inject=[a](uint16_t sport) {
            auto p=Data(sport); auto h=Header(p); InterfaceTag tag; tag.SetPortId(0); p->AddPacketTag(tag);
            a->SwitchReceiveFromDevice(a->Port(0),p,h);
        };
        for (auto& e:a->m_groups) {
            auto state=State(e.first,1,0); state[G::SERVICE]=12500000000;
            Check(a->ApplyRecord(e.second,state),"activation fixture state rejected");
        }
        a->Start();
        Simulator::Schedule(MicroSeconds(51),[a,inject]() {
            for (const auto& e:a->m_groups) Check(!e.second.target,"idle group reserved bandwidth");
            inject(10001); inject(10002);
        });
        Simulator::Schedule(MicroSeconds(52),[a]() {
            Check(a->m_flows.at(1).tx==1048 && a->m_flows.at(2).tx==1048,
                  "first arrivals waited for the next 50us control tick");
            double total=0; for (const auto& e:a->m_groups) total+=e.second.target;
            Check(total<=12500000000,"simultaneously reactivated groups exceeded port capacity");
        });
        // Explicit service stop must survive another idle-to-active transition.
        Simulator::Schedule(MicroSeconds(110),[a,inject]() {
            for (auto& e:a->m_groups) {
                auto state=State(e.first,2,110000); state[G::SERVICE]=0; state[G::FLAGS]|=G::PAUSED;
                Check(a->ApplyRecord(e.second,state),"activation pause rejected");
                a->UpdateSource(e.second);
            }
            a->CommitControls(); inject(10001); inject(10002);
        });
        Simulator::Schedule(MicroSeconds(130),[a]() {
            Check(a->m_flows.at(1).tx==1048 && a->m_flows.at(2).tx==1048,
                  "arrival bypassed remote service stop");
            for (auto& e:a->m_groups) {
                auto state=State(e.first,3,130000); state[G::SERVICE]=12500000000;
                Check(a->ApplyRecord(e.second,state),"activation resume rejected");
            }
        });
        Simulator::Schedule(MicroSeconds(151),[a]() {
            Check(a->m_flows.at(1).tx==2096 && a->m_flows.at(2).tx==2096,"activation resume failed");
        });
        Simulator::Schedule(MilliSeconds(21),[inject]() { inject(10001); });
        Simulator::Schedule(MicroSeconds(21001),[a]() {
            Check(a->m_flows.at(1).tx==2096,"arrival bypassed stale feedback stop");
        });
        Simulator::Stop(MicroSeconds(21002)); Simulator::Run(); Simulator::Destroy();
    }
    static void ProtectionRecovery() {
        auto a=Gateway(true); auto id=a->m_groups.begin()->first;
        a->m_mmu->SetBufferPool(10000000); a->m_mmu->SetIngressPool(10000000);
        a->m_mmu->SetEgressLosslessPool(10000000); a->m_mmu->reserveIngress[0][3]=10000000;
        auto state=State(id,1,0); state[G::SERVICE]=12500000000;
        Check(a->ApplyRecord(a->m_groups.at(id),state),"protection fixture state rejected");
        auto inject=[a]() {
            auto p=Data(); auto h=Header(p); p->AddPacketTag(InterfaceTag(0));
            a->SwitchReceiveFromDevice(a->Port(0),p,h);
        };
        a->Start();
        Simulator::Schedule(MicroSeconds(51),[a,id,inject]() {
            auto& g=a->m_groups.at(id); Check(!g.target,"idle fixture retained allocation");
            g.badResiduals=3; inject();
        });
        Simulator::Schedule(MicroSeconds(54),[a,id]() {
            const auto& g=a->m_groups.at(id);
            Check(g.candidate>0 && g.candidate<=g.capacity*a->m_config.conservativeFraction,
                  "idle allocation pinned a safe residual cap to zero");
            Check(a->m_flows.at(1).tx==1048,"residual recovery did not reach actual TX");
        });
        Simulator::Schedule(MicroSeconds(60),[a,id,inject]() {
            auto& g=a->m_groups.at(id); auto r=State(id,2,60000); r[G::SERVICE]=0; r[G::FLAGS]|=G::PAUSED;
            Check(a->ApplyRecord(g,r),"protected pause rejected"); a->ActivateSources(); inject();
            Check(!g.candidate && !g.target,"residual recovery bypassed service stop");
        });
        Simulator::Schedule(MicroSeconds(70),[a,id]() {
            Check(a->m_flows.at(1).tx==1048,"protected pause allowed actual TX");
            auto& g=a->m_groups.at(id); auto r=State(id,3,70000); r[G::SERVICE]=12500000000;
            Check(a->ApplyRecord(g,r),"protected resume rejected"); a->ActivateSources();
        });
        Simulator::Schedule(MicroSeconds(74),[a,id]() {
            auto& g=a->m_groups.at(id);
            Check(a->m_flows.at(1).tx==2096,"previous service stop pinned residual recovery to zero");
            // Peak protection must freeze the controller cap, even if allocation was zero.
            g.target=0; g.state[G::BUFFER]=70000; const double cap=g.candidate;
            a->UpdateSource(g);
            Check(g.peakLimited && g.candidate>0 && g.candidate<=cap,"peak cap used idle allocation");
            g.state[G::BUFFER]=1000; a->UpdateSource(g);
            Check(!g.candidate,"peak exhaustion was bypassed by residual recovery");
        });
        Simulator::Stop(MicroSeconds(75)); Simulator::Run(); Simulator::Destroy();
    }
    static void Measurements() {
        auto b=Gateway(false); auto& g=b->m_groups.begin()->second;
        auto p=Data();
        G::Admitted(PeekPointer(b),0,p,3,3);
        g.emptied=false;
        G::Dequeue(PeekPointer(b),0,p,3);
        G::Admitted(PeekPointer(b),0,p,3,3);
        Check(g.Queue()==1048 && g.emptied,"interior empty interval lost between nonzero endpoints");
        auto other=Data(12345); G::Admitted(PeekPointer(b),0,other,3,3);
        Check(g.Queue()==2096 && g.other==1048,"same-PG uncontrolled arrivals omitted");
        b->NotifyAdmissionDrop(0,3,other);
        Check(g.drops==1048 && g.Queue()==2096,"drop included as admitted queue bytes");
        Simulator::Destroy();
    }
    static void LocalLoop() {
        auto a=Gateway(true); auto id=a->m_groups.begin()->first;
        auto peer=DynamicCast<TestNode>(a->Port(0)->GetChannel()->GetDevice(1)->GetNode());
        Simulator::Schedule(MicroSeconds(200),[a,id]() {
            auto& g=a->m_groups.at(id); g.in=1200000; a->m_flows.at(1).in=1200000;
            a->UpdateBackpressure(g); Check(g.protecting && a->m_flows.at(1).nearCnps==1,"local growth did not initiate feedback");
        });
        Simulator::Schedule(MicroSeconds(400),[a,id]() {
            auto& g=a->m_groups.at(id); g.in+=100000; a->m_flows.at(1).in+=100000;
            a->UpdateBackpressure(g); Check(a->m_flows.at(1).nearCnps==1,"response wait did not suppress repeated actual CNP");
        });
        Simulator::Schedule(MicroSeconds(600),[a,id]() {
            auto& g=a->m_groups.at(id); g.in+=1000; g.tx+=100000;
            a->m_flows.at(1).in+=1000; a->m_flows.at(1).tx+=100000;
            a->UpdateBackpressure(g); Check(g.protecting && a->m_flows.at(1).nearCnps==1,"draining cleared episode or continued notifications");
        });
        Simulator::Schedule(MicroSeconds(800),[a,id]() {
            auto& g=a->m_groups.at(id); g.in+=100000; a->m_flows.at(1).in+=100000;
            a->UpdateBackpressure(g); Check(a->m_flows.at(1).nearCnps==2,"renewed pressure did not send actual CNP");
        });
        Simulator::Schedule(MicroSeconds(1000),[a,id]() {
            auto& g=a->m_groups.at(id); g.tx=g.in; a->m_flows.at(1).tx=g.tx;
            a->UpdateBackpressure(g); Check(!g.protecting,"low watermark did not release episode");
        });
        Simulator::Stop(MilliSeconds(2)); Simulator::Run();
        Check(peer->received.size()==2,"unexpected CNP wire count");
        for (const auto& h:peer->received)
            Check(h.l3Prot==0xff && h.cnp.fid==10001 && h.cnp.qIndex==3 && h.dip==1,"CNP targeted destination QP instead of sender");
        Simulator::Destroy();
    }
    static void DirectedBackpressure() {
        auto a=Gateway(true); auto id=a->m_groups.begin()->first;
        auto r=a->m_flows.at(1).reg; r.id=2; r.sport=10002; a->RegisterFlow(r);
        r.id=3; r.sport=10003; a->RegisterFlow(r);
        auto grow=[a,id](uint64_t first,uint64_t second) {
            a->m_flows.at(1).in+=first; a->m_flows.at(2).in+=second;
            auto& g=a->m_groups.at(id); g.in+=first+second; a->UpdateBackpressure(g);
        };
        Simulator::Schedule(MicroSeconds(200),[a,grow]() {
            grow(200000,100000);
            Check(!a->m_flows.at(1).nearCnps && !a->m_flows.at(2).nearCnps,
                  "small burst caused premature local feedback");
        });
        Simulator::Schedule(MicroSeconds(400),[a,grow]() {
            grow(800000,100000);
            Check(a->m_flows.at(1).nearCnps==1 && !a->m_flows.at(2).nearCnps,
                  "ordinary feedback was not limited to the largest contributor");
        });
        Simulator::Schedule(MicroSeconds(600),[a,grow]() {
            grow(1000,100000);
            Check(!a->m_flows.at(2).nearCnps,"group did not wait for previous QP response");
        });
        Simulator::Schedule(MicroSeconds(800),[a,grow]() {
            grow(1000,100000);
            Check(a->m_flows.at(1).nearCnps==1 && a->m_flows.at(2).nearCnps==1,
                  "renewed growth did not select the current contributor");
        });
        Simulator::Schedule(MicroSeconds(1000),[grow]() { grow(0,0); });
        Simulator::Schedule(MicroSeconds(1200),[a,grow]() {
            a->m_flows.at(2).lastNativeCnp=1150000;
            grow(10000,200000);
            Check(a->m_flows.at(1).nearCnps==1 && a->m_flows.at(2).nearCnps==1,
                  "waiting for the main contributor redirected CNP to a smaller contributor");
        });
        Simulator::Schedule(MicroSeconds(1400),[a,grow]() {
            a->m_config.bufferBytes=2000000;
            grow(100000,100000);
            Check(a->m_flows.at(1).nearCnps==2 && a->m_flows.at(2).nearCnps==2,
                  "emergency failed to notify all active contributors");
            Check(!a->m_flows.at(3).nearCnps,"idle flow received group feedback");
        });
        Simulator::Stop(MicroSeconds(1401)); Simulator::Run(); Simulator::Destroy();
    }
    static void NativePassThrough() {
        auto a=Gateway(true); auto peer=DynamicCast<TestNode>(a->Port(0)->GetChannel()->GetDevice(1)->GetNode());
        a->m_mmu->SetBufferPool(10000000); a->m_mmu->SetIngressPool(10000000);
        a->m_mmu->SetEgressLosslessPool(10000000); a->m_mmu->reserveIngress[0][3]=10000000;
        // Exercise ordinary SwitchReceive/forwarding; R5 does not intercept or rewrite CE.
        auto p=Data(10001,3); auto h=Header(p);
        InterfaceTag tag; tag.SetPortId(0); p->AddPacketTag(tag);
        a->SwitchReceiveFromDevice(a->Port(0),p,h);
        Simulator::Schedule(MicroSeconds(5),[a]() {
            auto p=Create<Packet>(); p->AddHeader(CnHeader(10001,3,3,1,1));
            Ipv4Header ip; ip.SetSource(Ipv4Address(2)); ip.SetDestination(Ipv4Address(1));
            ip.SetProtocol(0xff); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
            PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); p->AddPacketTag(InterfaceTag(0));
            auto h=Header(p); a->SwitchReceiveFromDevice(a->Port(0),p,h);
            Check(a->m_flows.at(1).lastNativeCnp==5000,"native CNP observation missing");
        });
        Simulator::Schedule(MicroSeconds(6),[a]() {
            CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
            h.pppProto=0x0021; h.l3Prot=0xfc; h.sip=2; h.dip=1;
            h.ack.sport=20001; h.ack.dport=10001; h.ack.pg=3;
            h.ack.seq=123; h.ack.flags=1<<qbbHeader::FLAG_CNP;
            auto p=Create<Packet>(); p->AddHeader(h); p->AddPacketTag(InterfaceTag(0));
            a->SwitchReceiveFromDevice(a->Port(0),p,h);
        });
        Simulator::Stop(MicroSeconds(10)); Simulator::Run();
        Check(peer->received.size()==3 && peer->received[0].GetIpv4EcnBits()==3,"A cleared existing CE");
        Check(peer->received[1].cnp.ecnBits==3 && peer->received[2].ack.seq==123 &&
              peer->received[2].ack.flags==(1<<qbbHeader::FLAG_CNP),"native feedback was rewritten or lost");
        Check(a->m_flows.at(1).nativeCnps==2 && a->m_flows.at(1).lastNativeCnp==6000,
              "ACK-CNP did not update native response wait");
        Check(a->m_flows.at(1).nearCnps==0,"upstream CE was converted into local CNP");
        Simulator::Destroy();
    }
    static void FeedbackAndActualSend() {
        auto a=CreateObject<G>(),b=CreateObject<G>(); a->m_enabled=b->m_enabled=true;
        a->SetNodeType(2); b->SetNodeType(2);
        auto wan=CreateObject<QbbChannel>(); wan->SetAttribute("Delay",TimeValue(MicroSeconds(100)));
        auto ad=Device(a,wan),bd=Device(b,wan);
        auto host=CreateObject<TestNode>(),sink=CreateObject<TestNode>(); host->SetNodeType(2); sink->SetNodeType(2);
        auto lanA=CreateObject<QbbChannel>(); Device(a,lanA); Device(host,lanA);
        auto lanB=CreateObject<QbbChannel>(); auto service=Device(b,lanB); Device(sink,lanB);
        auto route=[](Ptr<G> g,uint32_t ip,uint32_t port) { Ipv4Address dst(ip); g->AddTableEntry(dst,port); };
        route(a,1,1); route(a,2,0); route(a,G::ControlAddress(b->GetId()).Get(),0);
        route(b,1,0); route(b,2,1); route(b,G::ControlAddress(a->GetId()).Get(),0);
        for (auto g:{a,b}) {
            g->m_mmu->SetBufferPool(10000000); g->m_mmu->SetIngressPool(10000000);
            g->m_mmu->SetEgressLosslessPool(10000000);
            g->m_mmu->reserveIngress[g==a ? 1 : 0][3]=10000000;
        }
        G::Registration r; r.id=1; r.sip=1; r.dip=2; r.sport=10001; r.dport=20001;
        r.peer=b->GetId(); r.port=0; r.remotePort=1; r.propagation=wan->GetDelay()+bd->GetReceiveDelay();
        a->RegisterFlow(r); r.role=G::Role::Receiver; r.peer=a->GetId(); r.port=1; r.remotePort=0; b->RegisterFlow(r);
        a->Start(); b->Start();
        service->DoReceive(PausePacket(1011)); // Timer expiry must also trigger recovery feedback.
        Simulator::Schedule(MicroSeconds(100),[service]() { service->DoReceive(PausePacket(911)); });
        Simulator::Schedule(MicroSeconds(250),[b]() {
            Check(b->m_groups.begin()->second.seq==2,"PFC refresh caused redundant state feedback");
        });
        for (unsigned i=0;i<1000;++i) {
            auto p=Data(); auto h=Header(p); InterfaceTag tag; tag.SetPortId(1); p->AddPacketTag(tag);
            a->SwitchReceiveFromDevice(a->Port(1),p,h);
        }
        uint64_t stopped=0;
        Simulator::Schedule(MicroSeconds(450),[a,&stopped]() {
            const auto& g=a->m_groups.begin()->second;
            Check(g.have && !g.target && g.state[G::FLAGS]&G::PAUSED,"wire pause feedback did not stop actual injection");
            stopped=a->m_flows.at(1).tx;
        });
        Simulator::Schedule(MicroSeconds(900),[a,b,&stopped]() {
            Check(a->m_flows.at(1).tx==stopped,"A sent data while remote service was stopped");
            Check(b->m_controlTx>0 && a->m_controlRx>0,"stopped data also stopped independent feedback");
        });
        Simulator::Schedule(MicroSeconds(1175),[a,&stopped]() {
            Check(a->m_flows.at(1).tx>stopped,"resume waited for periodic feedback or source control tick");
        });
        Simulator::Stop(MilliSeconds(5)); Simulator::Run();
        Check(sink->received.size()==1000,"fresh feedback did not drain real data after service resume");
        Check(a->m_flows.at(1).tx==1048000 && b->m_flows.at(1).in==1048000 && b->m_flows.at(1).tx==1048000,
              "real A TX/B admission/B TX did not close");
        Simulator::Destroy();
    }
    static void ReceiverEcn() {
        auto g=Gateway(true); auto hw=CreateObject<RdmaHw>(); hw->SetNode(g);
        hw->m_ack_interval=1; hw->m_nic.emplace_back(g->Port(0)); hw->m_rtTable[1]={0};
        for (uint8_t ecn=0;ecn<4;++ecn) {
            auto p=Data(10001+ecn,ecn); auto h=Header(p); h.udp.seq=0; hw->ReceiveUdp(p,h);
            auto ack=g->Port(0)->GetRdmaQueue()->m_ackQ->Dequeue(); Check(bool(ack),"missing ACK");
            Check(bool(Header(ack).ack.flags&(1<<qbbHeader::FLAG_CNP))==(ecn==3),"receiver ECT/CE regression");
        }
        Simulator::Destroy();
    }
public:
    static void Run() { Replay(); Controller(); Service(); PauseAvailability(); Backpressure(); SharedShaper(); InterleavedGroups(); IdleActivation(); ProtectionRecovery(); Measurements(); LocalLoop(); DirectedBackpressure(); NativePassThrough(); FeedbackAndActualSend(); ReceiverEcn(); std::cout << "R5 mechanism regressions passed\n"; }
};
}
int main() { try { ns3::DciGatewayTestCase::Run(); return 0; } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
