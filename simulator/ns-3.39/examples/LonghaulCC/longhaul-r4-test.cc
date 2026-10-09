// Deterministic controller regressions, independent of research performance runs.
#include "ns3/dci-gateway-node.h"
#include "ns3/ppp-header.h"
#include "ns3/pause-header.h"
#include "ns3/qbb-channel.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include "ns3/qbb-helper.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/string.h"
#include "ns3/interface-tag.h"
#include "ns3/boolean.h"
#include "ns3/global-value.h"
#include <cmath>
#include <numeric>
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
    double Rate(uint32_t q) const { return m_shapers.at(q).rate; }
    void Prime() { for (auto& e:m_shapers) e.second.tokens=e.second.burst; }
    void Exhaust(uint32_t q) { auto& g=m_shapers.at(q).grant; g.sent+=g.remaining; g.remaining=0; }
    bool firstTx=true;
};
class DciGatewayTestCase {
    static void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
    static Ptr<DciGatewayNode> Gateway(bool source,uint32_t count=1) {
        auto g=CreateObject<DciGatewayNode>(); g->m_enabled=true; g->SetNodeType(2);
        auto dev=CreateObject<TestDevice>(); dev->SetQueue(CreateObject<BEgressQueue>());
        dev->SetDataRate(DataRate("100Gbps")); g->AddDevice(dev); dev->Attach(CreateObject<QbbChannel>());
        for (uint32_t i=1;i<=count;++i) {
            DciGatewayNode::Registration r; r.id=i; r.peer=999; r.port=0;
            r.role=source ? DciGatewayNode::Role::Source : DciGatewayNode::Role::Receiver;
            r.sip=1; r.dip=2; r.sport=10000+i; r.dport=20000+i;
            r.forwardDelay=MilliSeconds(5); r.feedbackRtt=MicroSeconds(200); r.peerPortBytesPerSec=12.5e9;
            g->RegisterFlow(r);
            auto& f=g->m_flows.at(i);
            if (source) f.source.history.Init(g->m_config.bin,.03);
            else f.receiver.window.Init(4,0,0);
        }
        dev->ConfigureProbeBucket(1e8,65536);
        return g;
    }
    static void Pause(Ptr<DciGatewayNode> g,uint32_t duration) {
        auto p=Create<Packet>(); p->AddHeader(PauseHeader(duration,0,3));
        Ipv4Header ip; ip.SetProtocol(0xfe); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); g->Port(0)->DoReceive(p);
    }
    using Record=DciGatewayNode::Record;
    static Record State(uint32_t id,uint64_t seq=1) {
        Record r{}; r[0]=id; r[1]=1; r[2]=seq; r[3]=Simulator::Now().GetNanoSeconds();
        r[5]=1000000000; r[6]=1000000000; r[7]=500000;
        r[10]=DciGatewayNode::HAS_DEMAND|DciGatewayNode::SENDABLE;
        r[11]=uint32_t(DciGatewayNode::ServiceSource::BudgetFallback); r[13]=r[3]; r[18]=seq;
        return r;
    }
    static Ptr<Packet> Wire(const std::vector<Record>& records,uint32_t version=4,uint32_t count=0,uint32_t kind=1) {
        std::vector<uint8_t> bytes;
        auto word=[&bytes](uint64_t w,int length) { for (int b=length-1;b>=0;--b) bytes.push_back((w>>(8*b))&255); };
        word(version,4); word(kind,4); word(count ? count : records.size(),4); word(kind==1 ? 152 : 64,4);
        for (const auto& r:records) for (size_t j=0;j<(kind==1 ? 19u : 8u);++j) word(r[j],8);
        auto p=Create<Packet>(bytes.data(),bytes.size());
        Ipv4Header ip; ip.SetSource(DciGatewayNode::ControlAddress(999)); ip.SetProtocol(249);
        ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip); PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
        return p;
    }
    static void Receive(Ptr<DciGatewayNode> g,Ptr<Packet> p) {
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(h);
        g->ReceiveRecords(p,h);
    }
    static Ptr<Packet> Data(uint8_t ecn=CustomHeader::ECN_CE,uint16_t sport=10001) {
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        h.pppProto=0x0021; h.l3Prot=0x11; h.sip=1; h.dip=2; h.m_tos=0xb8|ecn;
        h.udp.pg=3; h.udp.sport=sport; h.udp.dport=20001;
        auto p=Create<Packet>(1048-h.GetSerializedSize()); p->AddHeader(h);
        InterfaceTag tag; tag.SetPortId(0); p->AddPacketTag(tag); return p;
    }
    static CustomHeader Header(Ptr<const Packet> p) {
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        p->PeekHeader(h); return h;
    }
    static Ptr<TestNode> Connect(Ptr<DciGatewayNode> g) {
        auto peer=CreateObject<TestNode>(); peer->SetNodeType(2);
        auto rx=CreateObject<TestDevice>(); rx->SetQueue(CreateObject<BEgressQueue>());
        rx->SetDataRate(DataRate("100Gbps")); peer->AddDevice(rx);
        rx->Attach(DynamicCast<QbbChannel>(g->Port(0)->GetChannel()));
        for (uint32_t address:{1u,2u}) { Ipv4Address dst(address); g->AddTableEntry(dst,0); }
        g->m_mmu->SetBufferPool(10000000); g->m_mmu->SetIngressPool(10000000);
        g->m_mmu->SetEgressLosslessPool(10000000); g->m_mmu->reserveIngress[0][3]=10000000;
        auto d=g->Port(0);
        d->m_groupClassifier=MakeBoundCallback(&DciGatewayNode::Classify,PeekPointer(g),uint32_t(0));
        d->TraceConnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,PeekPointer(g),uint32_t(0)));
        d->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,PeekPointer(g),uint32_t(0)));
        return peer;
    }
    static void Disconnect(Ptr<DciGatewayNode> g) {
        auto d=g->Port(0);
        d->m_groupClassifier=Callback<uint32_t,Ptr<const Packet>,uint32_t>();
        d->TraceDisconnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,PeekPointer(g),uint32_t(0)));
        d->TraceDisconnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayNode::Dequeue,PeekPointer(g),uint32_t(0)));
    }
public:
    static void BoundaryRegression() {
        auto a=Gateway(true); auto peer=Connect(a); auto& f=a->m_flows.at(1);
        auto p=Data(); auto h=Header(p);
        GlobalValue::Bind("ChecksumEnabled",BooleanValue(true));
        a->ConsumeSourceCe(p,h);
        Check(p->GetSize()==1048 && h.m_tos==0xba && Header(p).m_tos==0xba,"U21 CE rewrite changed DSCP/length or left stale header");
        auto copy=p->Copy(); PppHeader ppp; Ipv4Header ip; ip.EnableChecksum();
        copy->RemoveHeader(ppp); copy->RemoveHeader(ip);
        Check(ip.IsChecksumOk(),"U21 rewritten IPv4 checksum invalid");
        GlobalValue::Bind("ChecksumEnabled",BooleanValue(false));
        a->UpdateSource();
        Check(f.upstreamCe==1 && f.upstreamCePending && f.nearPending && !f.Queue(),"U21 upstream CE needs local backlog");
        a->SendNearCnp(f);
        Check(f.nearCnps==1 && !f.upstreamCePending,"U22 sent feedback did not consume CE");
        auto second=Data(); auto sh=Header(second); a->ConsumeSourceCe(second,sh);
        a->UpdateSource();
        Check(f.upstreamCePending && !f.nearPending,"U22 rate limit lost pending CE");
        for (uint8_t ecn:{uint8_t(0),uint8_t(1),uint8_t(2)}) {
            auto packet=Data(ecn); auto parsed=Header(packet); a->ConsumeSourceCe(packet,parsed);
            Check(Header(packet).m_tos==(0xb8|ecn),"U24 non-CE changed");
        }
        auto other=Data(3,10099); auto oh=Header(other); a->ConsumeSourceCe(other,oh);
        Check(Header(other).GetIpv4EcnBits()==3 && a->ShouldMarkEcn(0,other),"U24 unregistered traffic changed");
        Check(!a->ShouldMarkEcn(0,Data()) && a->ShouldMarkEcn(1,Data()),"U23 source marking scope wrong");
        a->ClearTable(); Ipv4Address dst(2); a->AddTableEntry(dst,1);
        auto wrong=Data(); auto wh=Header(wrong); a->ConsumeSourceCe(wrong,wh);
        Check(Header(wrong).GetIpv4EcnBits()==3 && f.upstreamCe==2,"U24 wrong egress consumed CE");
        a->ClearTable(); for (uint32_t address:{1u,2u}) { Ipv4Address d(address); a->AddTableEntry(d,0); }
        a->m_config.ecnMode="path"; auto old=Data(); auto oldh=Header(old); a->ConsumeSourceCe(old,oldh);
        Check(Header(old).GetIpv4EcnBits()==3 && a->ShouldMarkEcn(0,old),"legacy path mode changed");
        a->m_config.ecnMode="source-boundary";
        Simulator::Schedule(MicroSeconds(50),[a]() {
            a->UpdateSource(); auto& f=a->m_flows.at(1);
            Check(f.nearPending && f.upstreamCePending,"U22 deferred CE never released");
            a->SendNearCnp(f); Check(f.nearCnps==2,"U22 feedback count wrong");
        });
        Simulator::Schedule(MicroSeconds(60),[a]() {
            auto p=Data(); auto h=Header(p); a->ConsumeSourceCe(p,h);
            auto& f=a->m_flows.at(1); f.lastCnp=Simulator::Now().GetNanoSeconds();
            a->UpdateSource(); Check(f.upstreamCePending && !f.nearPending,"U22 native suppression consumed pending CE");
        });
        Simulator::Schedule(MicroSeconds(110),[a]() {
            auto& f=a->m_flows.at(1); f.active=true; f.in=300000;
            a->UpdateSource();
            Check(f.nearPending && f.marking && f.upstreamCePending,"U22 combined CE/queue feedback missing");
            a->SendNearCnp(f); Check(f.nearCnps==3 && !f.upstreamCePending,"U22 combined feedback sent twice");
        });
        Simulator::Run();
        Check(peer->received.size()==3 && peer->received[0].l3Prot==0xff &&
            peer->received[0].cnp.fid==10001,"U21 near CNP not sent to host identity");
        Disconnect(a); Simulator::Destroy();

        // Exercise the real switch dequeue marking path in both roles.
        for (bool source:{true,false}) {
            auto g=Gateway(source); auto sink=Connect(g); g->m_ecnEnabled=true;
            g->m_mmu->ConfigEcn(0,0,0,1);
            for (int i=0;i<2;++i) {
                auto packet=Data(source ? 3 : 2); auto parsed=Header(packet);
                g->SwitchReceiveFromDevice(g->Port(0),packet,parsed);
                Check(parsed.GetIpv4EcnBits()==2,"U23 forwarding cache not cleared at A");
            }
            g->Port(0)->ApplyQueueControls({{8,1e9,false}});
            Simulator::Run();
            Check(sink->received.size()==2,"U23 data forwarding lost packets");
            for (const auto& h:sink->received)
                Check(h.GetIpv4EcnBits()==2,"U23 controlled A/B dequeue generated CE");
            if (!source) {
                auto retained=Data(); auto rh=Header(retained); g->ConsumeSourceCe(retained,rh);
                Check(rh.GetIpv4EcnBits()==3,"U24 B consumed CE");
                g->SwitchReceiveFromDevice(g->Port(0),retained,rh); Simulator::Run();
                Check(sink->received.size()==3 && sink->received.back().GetIpv4EcnBits()==3,
                    "U23 B failed to preserve existing CE on the wire");
                Check(g->ShouldMarkEcn(0,Data(2,10099)) && g->ShouldMarkEcn(1,Data(2)),
                    "U23 B suppression escaped controlled flow/egress scope");
                CustomHeader feedback; feedback.l3Prot=0xff; feedback.sip=2; feedback.dip=1;
                feedback.cnp.fid=10001; feedback.cnp.qIndex=3; feedback.cnp.ecnBits=3;
                g->ObserveFeedback(feedback); Check(g->m_flows.at(1).event,"U23 B ignored remote feedback");
            }
            Disconnect(g); Simulator::Destroy();
        }
        // Non-controlled traffic still takes the ordinary physical-queue ECN path.
        auto ordinary=Gateway(false); auto sink=Connect(ordinary); ordinary->m_ecnEnabled=true;
        ordinary->m_mmu->ConfigEcn(0,0,0,1); Pause(ordinary,10);
        for (int i=0;i<2;++i) {
            auto packet=Data(2,10099); auto h=Header(packet);
            ordinary->SwitchReceiveFromDevice(ordinary->Port(0),packet,h);
        }
        Simulator::Run();
        Check(sink->received.size()==2 && sink->received[0].GetIpv4EcnBits()==3,
            "U23 ordinary physical queue lost ECN marking");
        Disconnect(ordinary); Simulator::Destroy();
    }
    static void PermitAndStartupRegression() {
        auto a=Gateway(true,2); auto peer=Connect(a);
        a->SamplePorts(); a->UpdateDemand();
        for (uint32_t id:{1u,2u}) { auto r=State(id); r[5]=r[6]=0; r[10]=DciGatewayNode::SENDABLE; Receive(a,Wire({r})); }
        a->UpdateSource(); a->CommitControls();
        auto& f=a->m_flows.at(1);
        Check(f.source.mode==DciGatewayNode::TxMode::OptimisticStartup && f.target>0 &&
            !f.source.probeEpoch && !f.source.activeStateReceived,"U27 inactive zero STATE suppressed optimistic startup");
        Check(f.target==a->m_flows.at(2).target && f.target*2<=a->m_ports.at(0).available,
              "U27 optimistic startup bypassed shared port allocation");
        Simulator::Schedule(MicroSeconds(10),[a]() {
            auto packet=Data(2); auto h=Header(packet);
            a->SwitchReceiveFromDevice(a->Port(0),packet,h);
            Check(a->m_flows.at(1).tx==1048,"U25 empty-queue arrival waited for control tick");
        });
        Simulator::Schedule(MicroSeconds(20),[a]() {
            auto r=State(1,2); r[5]=r[6]=1e8; Receive(a,Wire({r}));
            a->UpdateSource(); a->CommitControls();
            auto& f=a->m_flows.at(1);
            Check(f.source.activeStateReceived && f.source.mode==DciGatewayNode::TxMode::Normal && f.target==1e8,
                  "U27 active budget failed to take over startup");
            Check(!f.Queue() && DynamicCast<TestDevice>(a->Port(0))->Rate(8)==1e8,"U25 empty queue lost normal permit");
        });
        Simulator::Schedule(MicroSeconds(30),[a]() {
            Pause(a,10); a->UpdateDemand(); a->UpdateSource(); a->CommitControls();
            auto packet=Data(2); auto h=Header(packet); a->SwitchReceiveFromDevice(a->Port(0),packet,h);
            Check(a->m_flows.at(1).tx==2096 && !a->m_flows.at(1).target,"U26 local pause bypassed");
        });
        Simulator::Schedule(MicroSeconds(25),[a]() {
            auto packet=Data(2); auto h=Header(packet);
            a->SwitchReceiveFromDevice(a->Port(0),packet,h);
            Check(a->m_flows.at(1).tx==2096,"U25 normal empty-queue arrival waited for control tick");
        });
        Simulator::Schedule(MicroSeconds(50),[a]() { a->UpdateDemand(); a->UpdateSource(); a->CommitControls(); });
        Simulator::Schedule(MicroSeconds(80),[a]() {
            Check(a->m_flows.at(1).tx==3144,"U26 paused tail failed to resume");
            a->CompleteFlow(1); a->UpdateDemand(); a->UpdateSource(); a->CommitControls();
            Check(!a->m_flows.at(1).target,"U26 completed empty flow retained permit");
        });
        Simulator::Schedule(MilliSeconds(21),[a]() {
            a->UpdateSource(); Check(!a->m_flows.at(2).source.candidate &&
                std::string(a->m_flows.at(2).source.reason)=="startup_timeout","U27 missing activity feedback allowed endless startup");
        });
        Simulator::Run(); Check(peer->received.size()==3,"U25 wire delivery mismatch");
        Disconnect(a); Simulator::Destroy();
        auto b=Gateway(false,2); b->SamplePorts();
        for (uint32_t id:{1u,2u}) {
            Record d{}; d[0]=id; d[1]=d[2]=d[7]=1; d[6]=DciGatewayNode::HAS_DEMAND;
            Receive(b,Wire({d},4,0,2));
        }
        b->UpdateDemand(); b->UpdateReceiver();
        Check(b->m_flows.at(1).budget==b->m_ports.at(0).available/2 &&
            !b->m_flows.at(1).probing,"U27 B still initialized at probe rate");
        // An idle/reactivated receiver must not obtain another line-rate start.
        auto& bf=b->m_flows.at(1); bf.active=false; bf.rate=1e8;
        b->UpdateDemand(); b->UpdateReceiver();
        Check(bf.rate==b->m_config.probeRate && bf.probing,"U27 receiver renewed optimistic start");
        Simulator::Destroy();

        auto paused=Gateway(true); paused->SamplePorts(); paused->UpdateDemand();
        auto r=State(1); r[5]=r[6]=0; r[10]=0; r[11]=0;
        Receive(paused,Wire({r})); paused->UpdateSource();
        Check(!paused->m_flows.at(1).target && !paused->m_flows.at(1).source.activeStateReceived,
              "U27 optimistic start bypassed explicit remote pause");
        r=State(1,2); r[5]=r[6]=0; Receive(paused,Wire({r})); paused->UpdateSource();
        Check(paused->m_flows.at(1).source.activeStateReceived && !paused->m_flows.at(1).source.candidate,
              "U27 active zero budget was replaced by optimistic permit");
        Simulator::Schedule(MilliSeconds(21),[paused]() {
            paused->UpdateSource(); Check(paused->m_flows.at(1).source.mode==DciGatewayNode::TxMode::Stopped,
                "U27 expired active state restarted optimistic injection");
        });
        Simulator::Run(); Simulator::Destroy();
    }
    static void PfcRefreshRegression() {
        auto sender=CreateObject<SwitchNode>(), peer=CreateObject<SwitchNode>();
        NodeContainer nodes; nodes.Add(sender); nodes.Add(peer);
        InternetStackHelper().Install(nodes);
        QbbHelper helper;
        helper.SetDeviceAttribute("DataRate",StringValue("100Gbps"));
        helper.SetChannelAttribute("Delay",TimeValue(MicroSeconds(1)));
        auto devices=helper.Install(nodes);
        Ipv4AddressHelper address; address.SetBase("10.0.0.0","255.255.255.0"); address.Assign(devices);
        auto tx=DynamicCast<QbbNetDevice>(devices.Get(0));
        auto rx=DynamicCast<QbbNetDevice>(devices.Get(1));
        uint32_t port=tx->GetIfIndex();
        sender->m_mmu->xoffUsed[port][3]=1;
        sender->CheckAndSendPfc(port,3);
        Simulator::Schedule(MicroSeconds(20),[sender,rx,port]() {
            Check(rx->IsQueuePaused(3),"ordinary PFC expired while congestion persisted");
            sender->m_mmu->xoffUsed[port][3]=0;
            sender->CheckAndSendResume(port,3);
        });
        Simulator::Schedule(MicroSeconds(30),[sender,rx]() {
            Check(!rx->IsQueuePaused(3),"ordinary PFC did not resume after drain");
            Check(sender->m_pfcRefresh.empty(),"PFC refresh survived explicit resume");
        });
        Simulator::Run(); Simulator::Destroy();
    }
    static void BatchObserved(TestDevice* d,Ptr<const Packet>,uint32_t) {
        if (d->firstTx) {
            Check(d->Rate(8)==1e8 && d->Rate(9)==1e6,"U07 send observed partially applied batch");
            Check(d->GetProbeSentBytes()==1048,"U08 counters not deducted before trace");
            d->firstTx=false;
        }
        Check(d->GetProbeSentBytes()<=1048+1048000*Simulator::Now().GetSeconds()+1e-6,
              "U08 shared probe envelope exceeded");
    }
    static void AdmissionRegression() {
        auto g=Gateway(false); auto d=g->Port(0);
        d->m_groupClassifier=MakeBoundCallback(&DciGatewayNode::Classify,PeekPointer(g),uint32_t(0));
        d->TraceConnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,PeekPointer(g),uint32_t(0)));
        Ipv4Address dst(2); g->AddTableEntry(dst,0);
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        h.pppProto=0x0021; h.l3Prot=0x11; h.sip=1; h.dip=2;
        h.udp.pg=3; h.udp.sport=10001; h.udp.dport=20001;
        auto packet=Create<Packet>(1048-h.GetSerializedSize()); packet->AddHeader(h);
        InterfaceTag tag; tag.SetPortId(0); packet->AddPacketTag(tag);
        g->m_mmu->SetBufferPool(0);
        g->SendToDev(packet->Copy(),h);
        Check(g->m_admissionDropPackets==1 && g->m_flows.at(1).in==0,"U18 MMU drop counted as admitted");
        g->m_mmu->SetBufferPool(1000000); g->m_mmu->SetIngressPool(1000000);
        g->m_mmu->SetEgressLosslessPool(1000000); g->m_mmu->reserveIngress[0][3]=1000000;
        g->SendToDev(packet->Copy(),h);
        Check(g->m_flows.at(1).in==1048 && d->GetQueue()->GetLogicalBytes(8)==1048,"U18 successful admission counter");
        d->TraceDisconnectWithoutContext("QbbAdmitted",MakeBoundCallback(&DciGatewayNode::Admitted,PeekPointer(g),uint32_t(0)));
        d->m_groupClassifier=Callback<uint32_t,Ptr<const Packet>,uint32_t>();
        Simulator::Destroy();
    }
    static void DeviceRegression() {
        auto a=CreateObject<TestNode>(),b=CreateObject<TestNode>(); a->SetNodeType(2); b->SetNodeType(2);
        auto tx=CreateObject<TestDevice>(),rx=CreateObject<TestDevice>();
        auto ch=CreateObject<QbbChannel>();
        for (auto d:{tx,rx}) { d->SetQueue(CreateObject<BEgressQueue>()); d->SetDataRate(DataRate("1Gbps")); }
        a->AddDevice(tx); b->AddDevice(rx); tx->Attach(ch); rx->Attach(ch);
        tx->ConfigureGroupShaper(8,3,0,1048); tx->ConfigureGroupShaper(9,4,0,1048);
        tx->ConfigureProbeBucket(1048000,1048);
        tx->GrantProbeBytes(8,1,2200); tx->GrantProbeBytes(9,1,2200);
        // Raw queue setup avoids the classifier; real device dequeue exercises tokens and link.
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        h.pppProto=0x0021; h.l3Prot=0x11; h.udp.pg=3;
        for (int i=0;i<3;++i) for (uint32_t q:{8,9}) {
            auto p=Create<Packet>(1048-h.GetSerializedSize()); p->AddHeader(h); tx->GetQueue()->Enqueue(p,q);
        }
        tx->Prime();
        tx->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&DciGatewayTestCase::BatchObserved,PeekPointer(tx)));
        tx->ApplyQueueControls({{8,1e8,true},{9,1e6,true}});
        Check(tx->Rate(8)==1e8 && tx->Rate(9)==1e6,"batch did not apply every rate");
        Simulator::Schedule(MicroSeconds(500),[tx]() {
            Check(tx->GetProbeSentBytes()<=1048,"shared burst exceeded");
            tx->GrantProbeBytes(8,1,9000); // Must not reset an existing epoch.
        });
        Simulator::Schedule(MilliSeconds(5),[tx]() {
            auto a=tx->GetProbeCounters(8),b=tx->GetProbeCounters(9);
            Check(a.sent==2096 && b.sent==2096 && a.remaining==104 && b.remaining==104,
                  "non-frame-multiple flow allowance was exceeded or starved");
            tx->ApplyQueueControls({{8,0,false},{9,0,false}});
            tx->ApplyQueueControls({{8,1e7,true},{9,1e7,true}});
            Check(tx->GetProbeCounters(8).remaining==104,"mode switch refreshed allowance");
            tx->ApplyQueueControls({{8,1e7,false},{9,1e7,false}});
        });
        Simulator::Schedule(MilliSeconds(6),[tx]() {
            Check(!tx->GetQueue()->GetLogicalBytes(8) && !tx->GetQueue()->GetLogicalBytes(9),"normal traffic consumed probe allowance");
            Check(tx->GetProbeSentBytes()==4192,"normal sends counted as probe");
        });
        Simulator::Run(); Simulator::Destroy();
    }
    static void Invalid(const std::string& name) {
        if (name=="boundary-no-near" || name=="boundary-no-ecn" || name=="ecn-mode" || name=="startup-mode") {
            auto g=CreateObject<DciGatewayNode>(); DciGatewayNode::Config c;
            if (name=="boundary-no-near") c.nearCnp=false;
            if (name=="boundary-no-ecn") c.shaperEcn=false;
            if (name=="ecn-mode") c.ecnMode="invalid";
            if (name=="startup-mode") c.startupMode="invalid";
            g->Configure(c,"/tmp/r4-invalid-must-not-create");
            throw std::runtime_error("invalid R4 configuration accepted");
        }
        auto g=Gateway(true,name=="queues" ? 120 : 1);
        if (name=="batch") {g->Port(0)->ApplyQueueControls({}); return;}
        if (name=="capacity") {g->Port(0)->ApplyQueueControls({{8,1e20,false}}); return;}
        if (name=="timeout") {g->m_config.timeout=.001; g->Start(); return;}
        auto r=g->m_flows.at(1).reg;
        if (name=="identity") r.id=2;
        else if (name=="queues") {r.id=121;r.sport=10121;r.dport=20121;}
        else if (name=="mixed") {r.id=2;r.sport=10002;r.dport=20002;r.role=DciGatewayNode::Role::Receiver;}
        g->RegisterFlow(r);
        throw std::runtime_error("invalid registration was accepted");
    }
    static void Run() {
        PfcRefreshRegression(); DeviceRegression(); AdmissionRegression();
        BoundaryRegression(); PermitAndStartupRegression();
        auto rates=DciGatewayNode::Allocate({20,80},100);
        Check(rates==std::vector<double>({20,80}),"U02 capped allocation");
        Check(DciGatewayNode::Allocate({100,100},100)==std::vector<double>({50,50}),"U02 equal allocation");
        Check(DciGatewayNode::Allocate({},100).empty(),"U02 empty allocation");
        Check(DciGatewayNode::Allocate({0,0},100)==std::vector<double>({0,0}),"U02 zero allocation");
        Check(DciGatewayNode::Allocate({100,100,100,100},100)==std::vector<double>({25,25,25,25}),"U03 group bias");
        auto many=DciGatewayNode::Allocate(std::vector<double>(120,1e12),12.5e9);
        Check(std::accumulate(many.begin(),many.end(),0.0)<=12.5e9,"U03 full port floating-point overshoot");
        auto queue=CreateObject<BEgressQueue>(); queue->ConfigureLogicalQueue(8,3); queue->ConfigureLogicalQueue(9,4);
        queue->Enqueue(Create<Packet>(100),8); queue->Enqueue(Create<Packet>(200),9); queue->Enqueue(Create<Packet>(10),0);
        bool paused[8]={}; paused[3]=true;
        Check(queue->DequeueRR(paused,{8,9})->GetSize()==10,"U19 control priority");
        Check(queue->DequeueRR(paused)->GetSize()==200,"U19 paused PG blocked unrelated PG");
        Check(!queue->DequeueRR(paused),"U19 PG pause bypassed");
        paused[3]=false; Check(queue->DequeueRR(paused)->GetSize()==100,"U19 lost paused packet");

        DciGatewayNode::ServiceWindow window; window.Init(2,0,0);
        window.AccountUntil(10); window.backlogged=true; window.AccountUntil(30);
        window.bytes=200; window.backlogged=false; window.Close(50,10);
        Check(window.observation.busy==20 && window.observation.bytes==200 && window.observation.pause==10,
              "U05 busy includes paused/token-blocked time");
        window.bytes=100; window.Close(100,10);
        Check(window.observation.busy==20 && window.observation.bytes==300,"U06 boundary packet missing");
        window.Close(150,10);
        Check(window.observation.begin==50 && window.observation.busy==0 && window.observation.bytes==100,"U05 ring expiration");

        DciGatewayNode::History history; history.Init(.001,.01);
        history.Add(1500000,1000); history.Advance(2000000);
        Check(std::abs(history.Predict(0,0,2000000,0,500000)-500)<1e-6,"U13 idle drain erases future impulse");
        Check(std::abs(history.Predict(1500000,0,2000000,0,0)-500)<1e-6,"U13 partial boundary bin");
        history.Advance(100000000); Check(!history.Covers(0,100000000),"U13 expired history returned zero");

        auto a=Gateway(true,3); auto b=Gateway(false,2);
        Receive(a,Wire({State(2)}));
        Check(a->m_flows.at(2).source.have && !a->m_flows.at(1).source.have,"U15 independent records wait for missing flow");
        auto bad=State(1); bad[1]=2;
        Receive(a,Wire({bad,State(3)}));
        Check(!a->m_flows.at(1).source.have && a->m_flows.at(3).source.have,"U14 bad record blocked good record");
        auto rejected=a->m_rejected;
        Receive(a,Wire({State(2)})); Receive(a,Wire({State(1)},3)); Receive(a,Wire({State(1)},4,UINT32_MAX));
        Check(a->m_rejected==rejected+3,"U14 sequence/version/length validation");
        Receive(a,Wire({State(1),State(1,2)}));
        Check(a->m_flows.at(1).source.state[2]==1,"U14 duplicate flow applied twice");

        for (unsigned field:{3u,6u,9u,10u,11u,14u,16u,17u,18u}) {
            auto malformed=State(1,5);
            if (field==3) malformed[3]++;
            else if (field==6) malformed[6]=malformed[5]+1;
            else if (field==9) malformed[9]=1;
            else if (field==10) malformed[10]=32;
            else if (field==11) malformed[11]=99;
            else if (field==18) malformed[18]=0;
            else malformed[field]=1;
            auto before=a->m_rejected; Receive(a,Wire({malformed}));
            Check(a->m_rejected==before+1 && a->m_flows.at(1).source.state[2]==1,"U14 invalid field changed accepted state");
        }
        auto batchB=Gateway(false,25),batchA=Gateway(true,25);
        for (auto& e:batchB->m_flows) e.second.reg.peer=batchA->GetId();
        for (auto& e:batchA->m_flows) e.second.reg.peer=batchB->GetId();
        batchB->BuildReports();
        Check(batchB->m_reports.size()==3,"U14 STATE MTU batching");
        for (auto packet:batchB->m_reports) {
            Check(packet->GetSize()<=1502,"U14 oversized control frame");
            Receive(batchA,packet->Copy());
        }
        for (auto& e:batchA->m_flows) Check(e.second.source.have,"U14 serialized record round trip");
        auto& f=a->m_flows.at(1); f.in=1048; f.active=true; a->m_ports.at(0).available=12.5e9;
        auto& state=f.source.state; state=State(1); state[4]=state[8]=1000000; state[10]|=DciGatewayNode::BACKLOG;
        a->m_config.reconstruct=false; a->UpdateSource(); auto old=f.source.candidate;
        a->m_config.reconstruct=true; a->UpdateSource();
        Check(f.source.candidate>old && f.source.covered,"snapshot/history changed more than queue estimate");
        state[6]=0; a->UpdateSource();
        Check(f.source.mode==DciGatewayNode::TxMode::Normal && f.source.candidate==0,"U12 recovery bypassed predicted drain");
        state[4]=state[8]=0; a->UpdateSource();
        Check(f.source.mode==DciGatewayNode::TxMode::RecoveryProbe && f.source.pendingGrant,"U09 missing first recovery grant");
        a->CommitControls(); auto epoch=f.source.probeEpoch;
        a->UpdateSource(); Check(f.source.probeEpoch==epoch,"U09 repeated state refreshed grant");
        DynamicCast<TestDevice>(a->Port(0))->Exhaust(f.queue);
        state[2]++; a->UpdateSource();
        Check(f.source.probeEpoch==epoch,"U09 new report without receipt renewed probe");
        state[8]=state[9]=65536; state[2]++; a->UpdateSource();
        Check(f.source.probeEpoch==epoch+1,"U09 confirmed receipt did not renew exhausted probe");
        a->CommitControls(); a->UpdateSource();
        Check(f.source.probeEpoch==epoch+1,"U09 receipt watermark consumed twice");
        state[10]&=~DciGatewayNode::SENDABLE; a->UpdateSource();
        Check(!f.source.candidate,"U11 remote pause ignored");
        state[10]|=DciGatewayNode::SENDABLE;
        a->CompleteFlow(1); a->CompleteFlow(1); state[6]=1000;
        a->UpdateSource(); Check(f.source.candidate>0 && f.closed,"U17 completion discarded queued tail");

        auto startup=Gateway(true); auto& sf=startup->m_flows.at(1);
        startup->m_config.startupMode="probe";
        sf.active=true; sf.in=1048; startup->m_ports.at(0).available=1e9;
        startup->UpdateSource(); startup->CommitControls();
        Check(sf.source.probeEpoch==1 && sf.source.startupGranted,"U10 startup grant absent");
        DynamicCast<TestDevice>(startup->Port(0))->Exhaust(sf.queue);
        sf.active=false; startup->UpdateSource(); sf.active=true; startup->UpdateSource();
        Check(sf.source.probeEpoch==1 && sf.source.candidate==0,"U09 idle restart refreshed startup");
        double sumHigh=0; for (auto& e:a->m_flows) sumHigh+=e.second.sourceHigh;
        Check(sumHigh<=a->m_config.sourceHigh,"U20 source watermark inflated");
        Record demand{}; demand[0]=1; demand[1]=1; demand[2]=1; demand[6]=DciGatewayNode::HAS_DEMAND; demand[7]=1;
        Receive(b,Wire({demand},4,0,2));
        b->SamplePorts(); b->UpdateDemand(); b->UpdateReceiver();
        Check(b->m_flows.at(1).budget>0 && !b->m_flows.at(1).in,"U10 no-data demand deadlock");
        auto& bf=b->m_flows.at(1); bf.rate=1e9;
        CustomHeader cnp(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        cnp.l3Prot=0xff; cnp.sip=2; cnp.dip=1; cnp.cnp.fid=10001; cnp.cnp.qIndex=3; cnp.cnp.ecnBits=3;
        for (int i=0;i<10;++i) b->ObserveFeedback(cnp);
        a->ObserveFeedback(cnp); Check(!f.event,"U04 source applied native decrease");
        Simulator::Schedule(MicroSeconds(50),[b]() {
            b->UpdateReceiver();
            Check(b->m_flows.at(1).decreases==1 && b->m_flows.at(2).decreases==0,"U04 feedback affected unrelated flow");
            Pause(b,100); b->UpdateDemand(); b->UpdateReceiver();
            Check(b->m_flows.at(1).budget==0 && b->m_flows.at(1).receiver.service==0,"U11 pause service");
        });
        Simulator::Schedule(MicroSeconds(160),[b]() {
            Check(!b->Port(0)->IsQueuePaused(3),"U19 pause expiry failed");
            b->UpdateDemand(); b->UpdateReceiver(); Check(b->m_flows.at(1).budget>0,"U11 resume budget");
            auto& f=b->m_flows.at(1);
            f.receiver.window.observation.busy=100000; f.receiver.window.observation.bytes=100;
            b->UpdateReceiver();
            Check(f.receiver.service==1000000 && f.receiver.serviceSource==DciGatewayNode::ServiceSource::BusyObserved,"U05 busy service");
            b->m_config.serviceMode="budget"; b->UpdateReceiver();
            Check(f.receiver.service==f.budget,"U05 budget service ablation");
            uint64_t q=0; for (auto& e:b->m_flows) q+=e.second.receiver.queueTarget;
            Check(q<=b->m_config.qref,"U20 inflated target queue");
        });
        Simulator::Schedule(MicroSeconds(170),[b]() {
            auto& f=b->m_flows.at(1);
            b->CompleteFlow(1); Check(!f.closed,"U17 receiver accepted direct host completion");
            Record d{}; d[0]=1; d[1]=1; d[2]=2; d[3]=170000;
            d[6]=DciGatewayNode::NO_MORE_HOST_DATA; d[7]=2;
            Receive(b,Wire({d},4,0,2)); Check(f.closed,"U17 DEMAND completion missing");
            f.in=1048; b->UpdateDemand(); b->UpdateReceiver();
            Check(f.active && f.budget>0,"U17 closed receiver discarded tail");
            d[2]=3; d[6]=DciGatewayNode::HAS_DEMAND; d[7]=3;
            auto rejected=b->m_rejected; Receive(b,Wire({d},4,0,2));
            Check(b->m_rejected==rejected+1 && f.closed,"U17 completion flag reversed");
        });
        Simulator::Schedule(MilliSeconds(30),[a]() {
            a->UpdateSource(); Check(!a->m_flows.at(1).source.candidate,"U11 stale state did not stop");
            auto fresh=State(1,10); fresh[8]=fresh[9]=65536; Receive(a,Wire({fresh})); a->UpdateSource();
            Check(a->m_flows.at(1).source.candidate>0,"U11 new state did not restore");
            auto& f=a->m_flows.at(1);
            f.source.history.bins.assign(1,0); a->UpdateSource();
            Check(f.source.fresh && !f.source.covered && !f.source.candidate,"U11 missing history did not stop fresh state");
            a->BuildReports(); Check(!a->m_reports.empty(),"U11 missing history stopped control reports");
        });
        Simulator::Run(); Simulator::Destroy();
        std::cout << "R4 controller and device regressions passed\n";
    }
};
}
int main(int argc,char** argv) {
    try {
        if (argc>1) ns3::DciGatewayTestCase::Invalid(argv[1]); else ns3::DciGatewayTestCase::Run();
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
