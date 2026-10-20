// Deterministic ECN checks through Qbb receive, routing, queues, dequeue and links.
#include "ns3/core-module.h"
#include "ns3/dci-gateway-node.h"
#include "ns3/qbb-helper.h"
#include "ns3/ppp-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/rdma-hw.h"
#include <iostream>
#include <map>
#include <set>
#include <sstream>
using namespace ns3;
namespace {
void Require(bool ok,const char* why) { NS_ABORT_MSG_IF(!ok,why); }
CustomHeader Parse(Ptr<const Packet> p) {
    CustomHeader h(7); p->PeekHeader(h); return h;
}
std::vector<uint8_t> Bytes(Ptr<const Packet> p) {
    std::vector<uint8_t> v(p->GetSize()); p->CopyData(v.data(),v.size()); return v;
}
void CheckChecksum(Ptr<const Packet> p) {
    auto copy=p->Copy(); PppHeader ppp; Ipv4Header ip; ip.EnableChecksum();
    copy->RemoveHeader(ppp); copy->RemoveHeader(ip);
    Require(ip.IsChecksumOk(),"invalid IPv4 checksum");
}
Ptr<Packet> PacketFor(uint16_t id,uint8_t ecn,uint32_t dst=0x0a020101,uint8_t protocol=17,uint32_t qpn=1,uint8_t pg=3) {
    CustomHeader h(7); h.pppProto=0x0021; h.sip=dst==0x0a010101 ? 0x0a020101 : 0x0a010101; h.dip=dst;
    h.l3Prot=protocol; h.ipid=id; h.m_tos=0xa8|ecn; h.m_ttl=61;
    h.ipv4Flags=CustomHeader::DONT_FRAGMENT; h.m_fragmentOffset=0;
    if (protocol==17) { h.udp.sport=20000; h.udp.dport=30000; h.udp.pg=pg;
        h.udp.destQpn=qpn; h.udp.seq=id; h.udp.payload_size=128+CustomHeader::GetUdpHeaderSize(); }
    else if (protocol==0xff) { h.cnp.qIndex=3; h.cnp.fid=20000; h.cnp.ecnBits=3; h.cnp.destQpn=1; }
    else if (protocol==0xfc || protocol==0xfd) { h.ack.sport=30000; h.ack.dport=20000;
        h.ack.pg=3; h.ack.flags=1; h.ack.destQpn=1; h.ack.seq=id; }
    else if (protocol==0xfe) { h.pfc.qIndex=7; h.pfc.time=1; h.pfc.qlen=0; }
    h.m_payloadSize=128+h.GetSerializedSize()-34;
    std::vector<uint8_t> payload(128); for (size_t i=0;i<payload.size();++i) payload[i]=uint8_t(i^id);
    auto p=Create<Packet>(payload.data(),payload.size()); p->AddHeader(h);
    PppHeader ppp; Ipv4Header ip; ip.EnableChecksum();
    p->RemoveHeader(ppp); p->RemoveHeader(ip); p->AddHeader(ip); p->AddHeader(ppp);
    return p;
}
class ObservedGateway : public DciGatewayNode {
public:
    std::set<uint16_t> clear;
    std::map<uint16_t,uint8_t> admitted;
    uint32_t changed=0, fastChanged=0;
    std::set<uint16_t> replaced;
    std::map<uint16_t,std::pair<int64_t,bool>> congestion;
    bool HandleLocalCongestion(uint32_t port,uint32_t pg,Ptr<Packet> p,CustomHeader& h) override {
        auto shared=p->Copy(); auto before=Bytes(shared); auto old=h.GetIpv4EcnBits(); auto id=h.ipid;
        bool covered=DciGatewayNode::HandleLocalCongestion(port,pg,p,h);
        auto after=Bytes(p); auto parsed=Parse(p);
        if (port==1 && h.l3Prot==17) congestion[id]={Simulator::Now().GetNanoSeconds(),covered};
        Require(Bytes(shared)==before,"Fast CNP modified a shared packet copy");
        Require(h.m_tos==parsed.m_tos && h.m_checksum==parsed.m_checksum,"dequeue Packet/CustomHeader disagree");
        CheckChecksum(p); Require(before.size()==after.size(),"dequeue changed length");
        const auto offset=PppHeader::GetStaticSize();
        for (size_t i=0;i<before.size();++i) {
            if (covered && (i==offset+1 || i==offset+10 || i==offset+11)) continue;
            if (before[i]!=after[i]) std::cerr<<"packet="<<id<<" byte="<<i<<" before="<<unsigned(before[i])<<" after="<<unsigned(after[i])<<" covered="<<covered<<std::endl;
            Require(before[i]==after[i],"dequeue changed unrelated field/payload");
        }
        if (covered) {
            replaced.insert(id);
            Require((h.m_tos&0xfc)==0xa8,"dequeue changed DSCP");
            Require(parsed.GetIpv4EcnBits()==(old==3 ? 2 : old),"bad Fast CNP ECN replacement");
            fastChanged+=old==3;
        }
        return covered;
    }
    bool PrepareDataAdmission(Ptr<Packet> p,CustomHeader& h,uint32_t in,uint32_t out,uint32_t pg) override {
        auto before=Bytes(p); auto id=h.ipid;
        bool ok=DciGatewayNode::PrepareDataAdmission(p,h,in,out,pg);
        if (!id) return ok; // Generated R5 protocol packets.
        Require(ok,"unexpected admission rejection");
        auto after=Bytes(p); auto parsed=Parse(p);
        Require(h.m_tos==parsed.m_tos && h.m_checksum==parsed.m_checksum,"Packet/CustomHeader disagree");
        CheckChecksum(p); Require(before.size()==after.size(),"packet length changed");
        const auto offset=PppHeader::GetStaticSize();
        for (size_t i=0;i<before.size();++i) {
            if (clear.count(id) && (i==offset+1 || i==offset+10 || i==offset+11)) continue;
            Require(before[i]==after[i],"unrelated header, DSCP or payload changed");
        }
        Require((h.m_tos&0xfc)==0xa8,"DSCP changed");
        if (clear.count(id)) { Require(parsed.GetIpv4EcnBits()==2,"CE was not reset to ECT(0)"); ++changed; }
        admitted[id]=parsed.GetIpv4EcnBits(); return ok;
    }
};
class Sink : public SwitchNode {
public:
    std::map<uint16_t,uint8_t> received;
    uint32_t fastReceived=0,fastPeer=0x0a020101,fastQpn=1;
    std::map<uint32_t,uint32_t> fastByQpn;
    bool SwitchReceiveFromDevice(Ptr<NetDevice>,Ptr<Packet> p,CustomHeader& h) override {
        if (h.ipid) { CheckChecksum(p); received[h.ipid]=h.GetIpv4EcnBits(); }
        if (h.l3Prot==0xf9) {
            auto copy=p->Copy(); copy->RemoveAtStart(h.GetSerializedSize()); R5Header control;
            if (copy->RemoveHeader(control) && control.type==R5Header::FAST_CNP) {
                Require(control.v[R5Header::DIP]==fastPeer && (!fastQpn || control.v[R5Header::QPN]==fastQpn),"Fast CNP lost original receiver identity");
                Require(h.sip!=fastPeer,"Fast CNP impersonates receiver"); ++fastReceived; ++fastByQpn[control.v[R5Header::QPN]];
            }
        }
        return true;
    }
};
std::map<uint16_t,uint8_t> aTx,bTx;
void Tx(std::map<uint16_t,uint8_t>* values,Ptr<const Packet> p,uint32_t) {
    auto h=Parse(p); if (h.ipid) { CheckChecksum(p); (*values)[h.ipid]=h.GetIpv4EcnBits(); }
}
void Route(Ptr<SwitchNode> node,uint32_t address,uint32_t port) {
    Ipv4Address ip(address); node->AddTableEntry(ip,port);
}
void Inject(Ptr<NetDevice> device,Ptr<Packet> p) {
    DynamicCast<QbbNetDevice>(device)->Receive(p);
}
void Run(const std::string& prefix,bool budgetTest=false) {
    aTx.clear(); bTx.clear();
    auto a=CreateObject<ObservedGateway>(),b=CreateObject<ObservedGateway>();
    auto upstream=CreateObject<Sink>(),downstream=CreateObject<Sink>(),local=CreateObject<Sink>();
    QbbHelper helper; helper.SetDeviceAttribute("DataRate",DataRateValue(DataRate("1Gbps")));
    helper.SetChannelAttribute("Delay",TimeValue(MicroSeconds(1)));
    helper.Install(a,upstream); helper.Install(a,b); helper.Install(a,local);
    helper.SetDeviceAttribute("DataRate",DataRateValue(DataRate("100Mbps")));
    helper.Install(b,downstream);
    // A ports: 0=DC ingress, 1=WAN, 2=local. B ports: 0=WAN, 1=DC.
    for (auto g:{a,b}) {
        g->SetAttribute("EcnEnabled",BooleanValue(true)); g->SetAttribute("AckHighPrio",UintegerValue(1));
        for (uint32_t port=0;port<g->GetNDevices();++port) g->m_mmu->ConfigEcn(port,1000,2000,1);
    }
    for (uint32_t ip:{0x0a020101u,DciGatewayNode::ControlAddress(b->GetId()).Get()}) Route(a,ip,1);
    Route(a,0x0a010101,0); Route(a,0x0a010102,2);
    for (uint32_t ip:{0x0a010101u,DciGatewayNode::ControlAddress(a->GetId()).Get()}) Route(b,ip,0);
    Route(b,0x0a020101,1);
    DciGatewayNode::Config c; c.initialDelay=.000001; c.period=.0001;
    c.controlBurstBytes=4096; c.maxReports=1;
    if (!budgetTest) c.controlFraction=1; // Isolate queue refusal from the separately tested shared control budget.
    a->Configure(c,prefix); b->Configure(c,prefix);
    a->SetPeer(b->GetId(),1,2048); b->SetPeer(a->GetId(),0,2048);
    a->Start(); b->Start();
    DynamicCast<QbbNetDevice>(a->GetDevice(1))->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&Tx,&aTx));
    DynamicCast<QbbNetDevice>(b->GetDevice(1))->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&Tx,&bTx));
    if (!budgetTest) {
        a->clear.insert(14);
        for (uint16_t id=20;id<36;++id) a->clear.insert(id);
        for (uint16_t id=110;id<114;++id) a->clear.insert(id);
    }
    auto send=[&](uint32_t us,Ptr<NetDevice> dev,Ptr<Packet> p) {
        Simulator::Schedule(MicroSeconds(us),&Inject,dev,p);
    };
    send(100,a->GetDevice(0),PacketFor(1,3)); // Empty A queue, no registration/near CNP.
    for (uint16_t id=2;id<=4;++id) send(100+50*id,a->GetDevice(0),PacketFor(id,id-2));
    send(350,a->GetDevice(0),PacketFor(5,3,0x0a010102)); // DC-local flow.
    send(400,b->GetDevice(1),PacketFor(6,3,0x0a010101)); // B -> A.
    for (uint16_t id=7;id<=10;++id) {
        uint8_t protocol=id==7 ? 0xff : id==8 ? 0xfc : id==9 ? 0xfd : 0xf9;
        send(450+20*(id-7),id==10 ? a->GetDevice(0) : b->GetDevice(1),
             PacketFor(id,3,id==10 ? 0x0a020101 : 0x0a010101,protocol));
    }
    send(550,a->GetDevice(0),PacketFor(11,3,0x0a020101,0xfe)); // Device consumes PFC.
    send(575,a->GetDevice(0),PacketFor(12,3,0x0a010102,17,0)); // Unmanaged UDP.
    send(600,a->GetDevice(1),PacketFor(13,3)); // WAN ingress must not clear.
    {
        auto registerAt=[&](uint32_t us,uint64_t query,bool capable) {
            R5Header h; h.type=R5Header::REGISTER;
            h.v[R5Header::SIP]=0x0a010101; h.v[R5Header::DIP]=0x0a020101;
            h.v[R5Header::QPN]=1; h.v[R5Header::SRC_QPN]=1;
            h.v[R5Header::CONNECTION]=1; h.v[R5Header::QUERY_ID]=query; h.v[R5Header::SESSION]=1;
            auto dst=DciGatewayNode::ControlAddress(a->GetId());
            send(us,a->GetDevice(0),DciGatewayNode::ControlPacket(h,Ipv4Address(0x0a010101),dst));
            h.type=R5Header::REGISTER_CONFIRM; h.v[R5Header::SAMPLE]=MicroSeconds(us).GetNanoSeconds();
            h.v[R5Header::FLAGS]=capable ? R5Header::FAST_CNP_CAPABLE : 0;
            send(us+20,a->GetDevice(0),DciGatewayNode::ControlPacket(h,Ipv4Address(0x0a010101),dst));
        };
        Simulator::Schedule(MicroSeconds(620),[&] { a->m_mmu->ConfigEcn(1,0,0,1); });
        for (uint16_t id=70;id<74;++id) send(630,a->GetDevice(0),PacketFor(id,3)); // Not registered.
        registerAt(700,1,true);
        send(850,a->GetDevice(0),PacketFor(14,3)); // Inside a valid window, but no local congestion.
        for (uint16_t id=110;id<114;++id) send(880,a->GetDevice(0),PacketFor(id,3)); // Expired original window must renew.
        registerAt(890,2,false); // Revoke support while the renewed window is still live.
        for (uint16_t id=130;id<134;++id) send(920,a->GetDevice(0),PacketFor(id,3));
        registerAt(950,3,true);
        for (uint16_t id=120;id<124;++id) send(854,a->GetDevice(0),PacketFor(id,3,0x0a020101,17,2)); // Another QP cannot borrow coverage.
        for (uint16_t id=60;id<64;++id) send(1000,a->GetDevice(0),PacketFor(id,2)); // Fresh CNP, ECT unchanged.
        Simulator::Schedule(MicroSeconds(1090),[&] {
            DynamicCast<QbbNetDevice>(a->GetDevice(0))->GetQueue()->SetAttribute("MaxBytes",DoubleValue(1));
        });
        for (uint16_t id=80;id<84;++id) send(1100,a->GetDevice(0),PacketFor(id,3)); // Real control queue rejects submission.
        Simulator::Schedule(MicroSeconds(1150),[&] {
            DynamicCast<QbbNetDevice>(a->GetDevice(0))->GetQueue()->SetAttribute("MaxBytes",DoubleValue(1024*1024));
        });
        send(812,a->GetDevice(1),PacketFor(90,3)); // Wrong ingress during local congestion.
        send(812,a->GetDevice(0),PacketFor(91,2));
        registerAt(1400,4,false);
        Simulator::Schedule(MicroSeconds(1440),[&] { a->m_mmu->ConfigEcn(1,0,0,1); });
        for (uint16_t id=100;id<104;++id) send(1450,a->GetDevice(0),PacketFor(id,3)); // Explicitly lacks capability.
    }
    // Congestion only at A: force normal marking and verify it reaches B and sink.
    Simulator::Schedule(MicroSeconds(800),[&] { a->m_mmu->ConfigEcn(1,0,0,1); });
    for (uint16_t id=20;id<40;++id) {
        send(810,a->GetDevice(0),PacketFor(id,id<36 ? 3 : 2));
    }
    // Congestion only at B: A is uncongested, B must still add CE.
    Simulator::Schedule(MicroSeconds(1200),[&] {
        a->m_mmu->ConfigEcn(1,1000,2000,1); b->m_mmu->ConfigEcn(1,0,0,1);
    });
    for (uint16_t id=40;id<56;++id) send(1210,a->GetDevice(0),PacketFor(id,2));
    Simulator::Stop(MilliSeconds(2)); Simulator::Run();
    Require(a->admitted.at(1)==3,"low-queue ingress policy failed");
    Require(downstream->received.at(1)==3,"low-queue end-to-end policy failed");
    for (uint16_t id=2;id<=4;++id) Require(downstream->received.at(id)==id-2,"non-CE changed");
    Require(local->received.at(5)==3 && local->received.at(12)==3,"local/unmanaged CE changed");
    Require(upstream->received.at(6)==3 && a->admitted.at(13)==3,"wrong direction CE changed");
    for (uint16_t id=7;id<=9;++id) Require(upstream->received.at(id)==3,"return CNP/ACK/NACK CE changed");
    Require(downstream->received.at(10)==3,"R5 control CE changed");
    Require(!a->admitted.count(11),"PFC entered forwarding admission");
    uint32_t aNew=0,bNew=0;
    for (uint16_t id=20;id<40;++id) {
        Require(aTx.count(id) && downstream->received.count(id),"A congestion packet missing");
        aNew+=a->admitted.at(id)!=3 && aTx.at(id)==3;
        Require(downstream->received.at(id)==aTx.at(id),"A CE cleared at B/downstream");
    }
    Require(budgetTest ? aNew>0 : aNew==0,"A egress marking policy failed");
    for (uint16_t id=40;id<56;++id) bNew+=aTx.at(id)!=3 && bTx.at(id)==3;
    Require(bNew>0,"B marking was not exercised");
    Require(a->changed==(!budgetTest ? 21u : 0u) && b->changed==0,"unexpected clearing count");
    if (!budgetTest) {
        std::cout<<"Fast CNP covered packet IDs:"; for (auto id:a->replaced) std::cout<<' '<<id; std::cout<<std::endl;
        std::set<uint16_t> expected;
        for (uint16_t id=20;id<40;++id) expected.insert(id);
        for (uint16_t id=60;id<63;++id) expected.insert(id);
        for (uint16_t id=110;id<113;++id) expected.insert(id);
        Require(a->replaced==expected,"unexpected packet obtained window coverage");
        Require(a->fastChanged==0 && upstream->fastReceived==3,"Fast CNP submit/clear/actual delivery mismatch");
        Require(aTx.at(20)==2 && downstream->received.at(20)==2,"successful CNP failed to clear upstream CE");
        Require(aTx.at(21)==2 && aTx.at(61)==2,"coalesced CNP failed to cover subsequent congested packets");
        Require(aTx.at(14)==2,"upstream CE did not reuse the window without local congestion");
        Require(aTx.at(60)==2,"successful CNP added CE to ECT packet");
        Require(aTx.at(90)==3,"wrong ingress was cleared");
        for (uint16_t id:{70,71,72,73,80,81,82,83,100,101,102,103,120,121,122,123,130,131,132,133}) Require(aTx.at(id)==3,"unconfirmed/failed submission cleared CE");
    }
    if (budgetTest) Require(a->replaced.empty() && upstream->fastReceived==0 && aTx.at(20)==3,
                            "control budget failure cleared CE or sent Fast CNP");
    std::ofstream observations(prefix+".congestion.csv"); observations<<"packet,time_ns,covered\n";
    for (const auto& item:a->congestion) observations<<item.first<<','<<item.second.first<<','<<item.second.second<<'\n';
    std::ofstream meta(prefix+".json"); meta<<"{\"a\":"; a->WriteMetadata(meta); meta<<",\"b\":"; b->WriteMetadata(meta); meta<<"}\n";
    for (auto g:{a,b}) {
        Require(g->m_admissionDropPackets==0 && g->m_mmu->totalUsed==0,"drop/residual ingress MMU");
        Require(g->m_mmu->egressPoolUsed[0]+g->m_mmu->egressPoolUsed[1]==0,"residual egress MMU");
        for (uint32_t port=0;port<g->GetNDevices();++port)
            Require(DynamicCast<QbbNetDevice>(g->GetDevice(port))->GetQueue()->GetNBytesTotal()==0,"residual device queue");
    }
    std::cout<<"PASS "<<(budgetTest ? "fast-budget" : "fast-cnp")<<": cleared="<<(a->changed+a->fastChanged)<<", A newly marked="<<aNew<<", B newly marked="<<bNew
             <<"; Packet/CustomHeader, DSCP, checksum, payload, direction, local/unmanaged/control, PFC, no drops/residuals\n";
    Simulator::Destroy();
}
void RunSourceRoles(const std::string& prefix) {
    auto left=CreateObject<ObservedGateway>(),right=CreateObject<ObservedGateway>();
    auto dc1=CreateObject<Sink>(),dc2=CreateObject<Sink>(); dc2->fastPeer=0x0a010101;
    QbbHelper helper; helper.SetDeviceAttribute("DataRate",DataRateValue(DataRate("1Gbps")));
    helper.SetChannelAttribute("Delay",TimeValue(MicroSeconds(1)));
    helper.Install(left,dc1); helper.Install(left,right); helper.Install(right,dc2);
    // Left DC/WAN = 0/1; right WAN/DC = 0/1. Both act as source and destination.
    Route(left,0x0a010101,0); Route(left,0x0a010102,0); Route(left,0x0a020101,1);
    Route(right,0x0a010101,0); Route(right,0x0a020101,1);
    Route(left,DciGatewayNode::ControlAddress(right->GetId()).Get(),1);
    Route(right,DciGatewayNode::ControlAddress(left->GetId()).Get(),0);
    DciGatewayNode::Config c; c.initialDelay=.000001; c.period=.0001; c.controlFraction=1;
    for (auto g:{left,right}) {
        g->Configure(c,prefix); g->SetAttribute("EcnEnabled",BooleanValue(true));
        g->SetAttribute("AckHighPrio",UintegerValue(1));
        for (uint32_t port=0;port<g->GetNDevices();++port) g->m_mmu->ConfigEcn(port,1000,2000,1);
    }
    left->SetPeer(right->GetId(),1,2048); right->SetPeer(left->GetId(),0,2048);
    for (auto g:{left,right}) { g->Start(); }
    left->clear={201,203}; right->clear={202,204};
    std::map<uint16_t,uint8_t> leftWan,rightWan;
    DynamicCast<QbbNetDevice>(left->GetDevice(1))->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&Tx,&leftWan));
    DynamicCast<QbbNetDevice>(right->GetDevice(0))->TraceConnectWithoutContext("QbbDequeue",MakeBoundCallback(&Tx,&rightWan));
    auto send=[&](uint32_t us,Ptr<NetDevice> dev,Ptr<Packet> p) { Simulator::Schedule(MicroSeconds(us),&Inject,dev,p); };
    auto registration=[&](Ptr<ObservedGateway> g,uint32_t dc,uint32_t src,uint32_t dst) {
        R5Header h; h.type=R5Header::REGISTER;
        h.v[R5Header::SIP]=src; h.v[R5Header::DIP]=dst; h.v[R5Header::QPN]=1;
        h.v[R5Header::SRC_QPN]=1; h.v[R5Header::CONNECTION]=1; h.v[R5Header::QUERY_ID]=1;
        h.v[R5Header::SESSION]=1;
        send(50,g->GetDevice(dc),DciGatewayNode::ControlPacket(h,Ipv4Address(src),DciGatewayNode::ControlAddress(g->GetId())));
        h.type=R5Header::REGISTER_CONFIRM; h.v[R5Header::SAMPLE]=MicroSeconds(50).GetNanoSeconds();
        h.v[R5Header::FLAGS]=R5Header::FAST_CNP_CAPABLE;
        send(70,g->GetDevice(dc),DciGatewayNode::ControlPacket(h,Ipv4Address(src),DciGatewayNode::ControlAddress(g->GetId())));
    };
    registration(left,0,0x0a010101,0x0a020101); registration(right,1,0x0a020101,0x0a010101);
    auto pause=[] {
        auto p=PacketFor(0,0,0x0a020101,0xfe); auto h=Parse(p); p->RemoveHeader(h);
        h.pfc.qIndex=3; h.pfc.time=300; p->AddHeader(h); return p;
    };
    send(90,left->GetDevice(1),pause()); send(90,right->GetDevice(0),pause());
    send(100,left->GetDevice(0),PacketFor(201,3));
    send(100,right->GetDevice(1),PacketFor(202,3,0x0a010101));
    send(120,left->GetDevice(0),PacketFor(203,3));
    send(120,right->GetDevice(1),PacketFor(204,3,0x0a010101));
    send(130,left->GetDevice(0),PacketFor(211,1));
    send(130,right->GetDevice(1),PacketFor(212,1,0x0a010101));
    Simulator::Schedule(MicroSeconds(140),[&] {
        Require(dc1->fastReceived==1 && dc2->fastReceived==1,"upstream CE feedback waited for WAN dequeue");
        Require(!leftWan.count(201) && !rightWan.count(202),"WAN pause failed to hold source data");
        Require(left->admitted.at(201)==2 && right->admitted.at(202)==2,"source CE was not cleared at admission");
    });
    // CE arriving on WAN is remote-role data and must survive both physical gateways.
    send(200,right->GetDevice(0),PacketFor(220,3));
    send(200,left->GetDevice(1),PacketFor(221,3,0x0a010101));
    send(220,left->GetDevice(0),PacketFor(222,3,0x0a010102));
    for (uint16_t i=0;i<3;++i) {
        uint8_t protocol=i==0 ? 0xff : i==1 ? 0xfc : 0xfd;
        send(300+10*i,left->GetDevice(1),PacketFor(230+i,3,0x0a010101,protocol));
        send(300+10*i,right->GetDevice(0),PacketFor(233+i,3,0x0a020101,protocol));
    }
    Simulator::Schedule(MicroSeconds(380),[&] {
        left->m_mmu->ConfigEcn(1,0,0,1); right->m_mmu->ConfigEcn(0,0,0,1);
    });
    Simulator::Stop(MilliSeconds(2)); Simulator::Run();
    Require(left->changed==2 && right->changed==2,"source role was tied to a physical gateway");
    Require(dc1->fastReceived==2 && dc2->fastReceived==2,"shared window failed to renew for local congestion");
    for (uint16_t id:{201,203}) Require(leftWan.at(id)==2 && dc2->received.at(id)==2,"DC1->DC2 source/remote policy failed");
    for (uint16_t id:{202,204}) Require(rightWan.at(id)==2 && dc1->received.at(id)==2,"DC2->DC1 source/remote policy failed");
    Require(dc1->received.at(212)==1 && dc2->received.at(211)==1,"local ECN replacement changed ECT(1)");
    Require(dc2->received.at(220)==3 && dc1->received.at(221)==3 && dc1->received.at(222)==3,"remote or DC-local CE was cleared");
    for (uint16_t id:{230,231,232}) Require(dc1->received.at(id)==3,"DC1 return feedback was changed");
    for (uint16_t id:{233,234,235}) Require(dc2->received.at(id)==3,"DC2 return feedback was changed");
    for (auto g:{left,right}) {
        Require(g->m_admissionDropPackets==0 && g->m_mmu->totalUsed==0,"bidirectional drop/residual MMU");
        Require(g->m_mmu->egressPoolUsed[0]+g->m_mmu->egressPoolUsed[1]==0,"bidirectional residual egress MMU");
        for (uint32_t port=0;port<g->GetNDevices();++port)
            Require(DynamicCast<QbbNetDevice>(g->GetDevice(port))->GetQueue()->GetNBytesTotal()==0,"bidirectional residual queue");
    }
    std::ofstream meta(prefix+".json"); meta<<"{\"left\":"; left->WriteMetadata(meta); meta<<",\"right\":"; right->WriteMetadata(meta); meta<<"}\n";
    std::cout<<"PASS both source roles: immediate upstream CE CNP before paused WAN dequeue, shared window, local renewal, remote/native feedback preserved\n";
    Simulator::Destroy();
}
// Ordinary pressure must distinguish an accumulating QP, a fully drained QP,
// and a QP growing by only one packet per sample. No ECN signal enters this test.
void RunPressure(const std::string& prefix,bool emergency) {
    auto a=CreateObject<ObservedGateway>(),b=CreateObject<ObservedGateway>();
    auto source=CreateObject<Sink>(),sink=CreateObject<Sink>(); source->fastQpn=0;
    QbbHelper helper; helper.SetDeviceAttribute("DataRate",DataRateValue(DataRate("1Gbps")));
    helper.SetChannelAttribute("Delay",TimeValue(MicroSeconds(1)));
    helper.Install(a,source); helper.Install(a,b); helper.Install(b,sink);
    Route(a,0x0a010101,0); Route(a,0x0a020101,1);
    Route(b,0x0a010101,0); Route(b,0x0a020101,1);
    Route(a,DciGatewayNode::ControlAddress(b->GetId()).Get(),1);
    Route(b,DciGatewayNode::ControlAddress(a->GetId()).Get(),0);
    auto frame=PacketFor(1,2)->GetSize();
    DciGatewayNode::Config c; c.initialDelay=.000001; c.period=.0001;
    c.controlFraction=1; c.qref=0; c.sourceHigh=8*frame; c.sourceLow=2*frame; c.margin=frame;
    if (emergency) c.bufferBytes=10000;
    a->Configure(c,prefix); b->Configure(c,prefix);
    a->SetPeer(b->GetId(),1,frame); b->SetPeer(a->GetId(),0,frame);
    a->Start(); b->Start();
    auto send=[&](uint32_t us,Ptr<NetDevice> dev,Ptr<Packet> p) {
        Simulator::Schedule(MicroSeconds(us),&Inject,dev,p);
    };
    for (uint32_t qpn=1;qpn<=3;++qpn) {
        R5Header h; h.type=R5Header::REGISTER;
        h.v[R5Header::SIP]=0x0a010101; h.v[R5Header::DIP]=0x0a020101;
        h.v[R5Header::QPN]=qpn; h.v[R5Header::SRC_QPN]=qpn;
        h.v[R5Header::CONNECTION]=1; h.v[R5Header::QUERY_ID]=1; h.v[R5Header::SESSION]=1;
        auto address=DciGatewayNode::ControlAddress(a->GetId());
        send(50,a->GetDevice(0),DciGatewayNode::ControlPacket(h,Ipv4Address(0x0a010101),address));
        h.type=R5Header::REGISTER_CONFIRM; h.v[R5Header::SAMPLE]=MicroSeconds(50).GetNanoSeconds();
        h.v[R5Header::FLAGS]=R5Header::FAST_CNP_CAPABLE;
        send(70,a->GetDevice(0),DciGatewayNode::ControlPacket(h,Ipv4Address(0x0a010101),address));
    }
    for (uint8_t pg:{3,5}) {
        auto p=PacketFor(0,0,0x0a020101,0xfe); auto h=Parse(p); p->RemoveHeader(h);
        h.pfc.qIndex=pg; h.pfc.time=2000; p->AddHeader(h);
        send(90,a->GetDevice(1),p);
    }
    uint16_t id=1000;
    for (uint32_t sample=0;sample<6;++sample) {
        for (unsigned i=0;i<32;++i) send(100+200*sample,a->GetDevice(0),PacketFor(id++,2));
        send(110+200*sample,a->GetDevice(0),PacketFor(id++,2,0x0a020101,17,2,4));
        send(120+200*sample,a->GetDevice(0),PacketFor(id++,2,0x0a020101,17,3,5));
    }
    Simulator::Schedule(MicroSeconds(1300),[&] {
        Require(source->fastByQpn[1]>0,"accumulating flow received no pressure feedback");
        if (!emergency) {
            Require(source->fastByQpn[2]==0,"zero-contribution flow was punished by global pressure");
            Require(source->fastByQpn[3]==0,"single-packet sampling growth triggered pressure feedback");
        } else {
            Require(source->fastByQpn[2]>0 && source->fastByQpn[3]>0,"emergency protection lost its override");
        }
    });
    Simulator::Stop(MilliSeconds(5)); Simulator::Run();
    Require(sink->received.size()==204,"pressure test failed to deliver all data");
    for (auto g:{a,b}) {
        Require(g->changed==0 && g->fastChanged==0,"pressure-only test unexpectedly changed CE");
        Require(g->m_admissionDropPackets==0 && g->m_mmu->totalUsed==0,"pressure drop/residual MMU");
        Require(g->m_mmu->egressPoolUsed[0]+g->m_mmu->egressPoolUsed[1]==0,"pressure residual egress MMU");
        for (uint32_t port=0;port<g->GetNDevices();++port)
            Require(DynamicCast<QbbNetDevice>(g->GetDevice(port))->GetQueue()->GetNBytesTotal()==0,"pressure residual device queue");
    }
    std::ofstream meta(prefix+".json"); meta<<"{\"a\":"; a->WriteMetadata(meta); meta<<",\"b\":"; b->WriteMetadata(meta); meta<<"}\n";
    std::cout<<"PASS "<<(emergency ? "emergency" : "ordinary")<<" pressure: growing/zero/one-packet QP notifications="
             <<source->fastByQpn[1]<<'/'<<source->fastByQpn[2]<<'/'<<source->fastByQpn[3]<<"; 204 delivered, no drops/residuals\n";
    Simulator::Destroy();
}

}
int main(int argc,char** argv) {
    std::string output="r5-ecn-test"; CommandLine cmd; cmd.AddValue("output","output prefix",output); cmd.Parse(argc,argv);
    GlobalValue::Bind("ChecksumEnabled",BooleanValue(true)); CustomHeader::qpnEnabled=true; IntHeader::mode=IntHeader::NONE;
    Run(output+"-fast-cnp"); Run(output+"-fast-budget",true);
    RunSourceRoles(output+"-source-roles");
    RunPressure(output+"-pressure",false); RunPressure(output+"-emergency",true);
    // QP lookup is by original remote identity, not the switch IP or a coincident local QPN.
    auto hw=CreateObject<RdmaHw>(); hw->m_node=CreateObject<Node>(); hw->m_cc_mode=1; hw->m_fastCnpSupported=true; hw->m_rateOnFirstCNP=.5;
    auto qp=CreateObject<RdmaQueuePair>(3,Ipv4Address(0x0a010101),Ipv4Address(0x0a020101),20000,30000);
    qp->connected=true; qp->localQpn=9; qp->destQpn=1; qp->connectionGeneration=123;
    qp->m_rate=DataRate("1Gbps"); hw->m_txQpn[9]=qp;
    Require(!hw->ReceiveFastCnp(0x0a020102,1,123) && !hw->ReceiveFastCnp(0x0a020101,2,123) &&
            !hw->ReceiveFastCnp(0x0a020101,1,124),"wrong Fast CNP association accepted");
    hw->m_fastCnpSupported=false; Require(!hw->ReceiveFastCnp(0x0a020101,1,123),"unsupported host accepted Fast CNP");
    hw->m_fastCnpSupported=true; auto rate=qp->m_rate.GetBitRate();
    Require(hw->ReceiveFastCnp(0x0a020101,1,123) && qp->m_rate.GetBitRate()<rate,"Fast CNP did not reach DCQCN");
    Simulator::Destroy();
    std::cout<<"PASS Fast CNP remote QP lookup, stale association rejection and DCQCN response\n";
}
