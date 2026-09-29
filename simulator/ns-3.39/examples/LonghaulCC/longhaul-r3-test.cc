// Deterministic controller regressions, independent of research performance runs.
#include "ns3/dci-gateway-node.h"
#include "ns3/ppp-header.h"
#include "ns3/pause-header.h"
#include "ns3/qbb-channel.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace ns3 {
class DciGatewayTestCase {
    static void Check(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
    static Ptr<DciGatewayNode> Gateway(bool source,uint32_t count=1) {
        auto g=CreateObject<DciGatewayNode>(); g->m_enabled=true; g->SetNodeType(2);
        auto dev=CreateObject<QbbNetDevice>(); dev->SetQueue(CreateObject<BEgressQueue>());
        dev->SetDataRate(DataRate("100Gbps")); g->AddDevice(dev);
        dev->Attach(CreateObject<QbbChannel>());
        for (uint32_t i=1;i<=count;++i) {
            DciGatewayNode::Registration r; r.id=i; r.group=1; r.peer=999; r.port=0;
            r.source=source; r.sip=1; r.dip=2; r.sport=10000+i; r.dport=20000+i;
            r.delay=0.005; r.feedbackRtt=0.0002;
            g->RegisterFlow(r);
        }
        return g;
    }
    static void Pause(Ptr<DciGatewayNode> g,uint32_t duration) {
        auto p=Create<Packet>(); p->AddHeader(PauseHeader(duration,0,3));
        Ipv4Header ip; ip.SetProtocol(0xfe); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); g->Port(0)->DoReceive(p);
    }
    static void Snapshot(Ptr<DciGatewayNode> g,uint64_t seq,uint64_t part,uint64_t generation=1) {
        const uint64_t total=g->m_flows.size(), count=std::min(uint64_t(24),total-part*24);
        std::vector<uint64_t> words={3,1,1,1,seq,uint64_t(Simulator::Now().GetNanoSeconds()),0,1000000,1000000,
                                      0,part,(total+23)/24,total,count};
        for (uint64_t j=part*24+1;j<=part*24+count;++j) {
            words.push_back(j); words.push_back(generation); words.push_back(1000+j); words.push_back(1);
        }
        std::vector<uint8_t> bytes;
        for (auto w:words) for (int b=7;b>=0;--b) bytes.push_back((w>>(b*8))&255);
        auto p=Create<Packet>(bytes.data(),bytes.size());
        Ipv4Header ip; ip.SetSource(DciGatewayNode::ControlAddress(999));
        ip.SetDestination(DciGatewayNode::ControlAddress(g->GetId())); ip.SetProtocol(249);
        ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip); PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header); p->PeekHeader(h);
        g->ReceiveSnapshot(p,h);
    }
public:
    static void Run() {
        auto rates=DciGatewayNode::Allocate({20,60},72);
        Check(rates[0]==20 && rates[1]==52,"capped max-min allocation");
        rates=DciGatewayNode::Allocate({0,20,60},100);
        Check(rates[0]==0 && rates[1]==20 && rates[2]==60,"unused capacity must not exceed flow caps");
        auto queue=CreateObject<BEgressQueue>();
        queue->ConfigureLogicalQueue(8,3); queue->ConfigureLogicalQueue(9,3);
        queue->Enqueue(Create<Packet>(100),8); queue->Enqueue(Create<Packet>(200),9);
        bool paused[8]={};
        Check(queue->DequeueRR(paused,{8})->GetSize()==200,"limited flow blocked another logical queue");
        paused[3]=true; Check(!queue->DequeueRR(paused),"logical queue bypassed physical PG pause");
        paused[3]=false; Check(queue->DequeueRR(paused)->GetSize()==100,"paused logical queue lost packet");
        DciGatewayNode::History history; history.Init(0.001,0.01);
        history.Add(1500000,1000); history.Advance(2000000);
        Check(std::abs(history.Predict(0,0,2000000,0,500000)-500)<1e-6,
              "empty past service must not cancel a later burst");
        history.Advance(100000000); Check(!history.Covers(0,100000000),"expired history must be rejected");

        auto ablation=Gateway(true);
        auto& group=ablation->m_groups.at(1);
        group.have=true; group.snapshot.queue=1000000;
        group.snapshot.service=1000000000; group.snapshot.budget=1000000000;
        ablation->m_flows.at(1).budget=1000000000;
        ablation->m_config.reconstruct=false; ablation->UpdateSource();
        double oldTarget=group.target;
        Check(group.queueUsed==1000000 && group.fresh,"snapshot ablation changed freshness/queue semantics");
        ablation->m_config.reconstruct=true; ablation->UpdateSource();
        Check(group.queueUsed==group.predicted && group.target>oldTarget,"history and old snapshot did not isolate queue estimate");

        auto a=Gateway(true,25);
        Snapshot(a,1,1); Check(!a->m_groups.at(1).have,"partial state applied");
        Snapshot(a,1,0); Check(a->m_groups.at(1).have && a->m_flows.at(25).budget==1025,"atomic fragmented state");
        auto rejected=a->m_rejected; Snapshot(a,1,0); Check(a->m_rejected==rejected+1,"old sequence accepted");
        Snapshot(a,2,0,2); Check(a->m_flows.at(1).budget==1001,"wrong generation applied");
        Snapshot(a,3,0); Check(a->m_groups.at(1).snapshot.seq==1,"missing fragment changed current state");

        auto mixed=Gateway(false,2);
        mixed->m_flows.at(1).in=1048;
        mixed->m_flows.at(2).demand=true; mixed->m_groups.at(1).demandTime=0;
        mixed->UpdateReceiver();
        Check(mixed->m_groups.at(1).service>0 && mixed->m_groups.at(1).service<mixed->m_groups.at(1).budget,
              "idle flow budget incorrectly counted as backlogged service");
        auto b=Gateway(false); auto& f=b->m_flows.at(1);
        f.rate=1000000000; f.demand=true; b->m_groups.at(1).demandTime=0;
        CustomHeader cnp(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        cnp.l3Prot=0xff; cnp.sip=2; cnp.dip=1; cnp.cnp.fid=10001; cnp.cnp.qIndex=3; cnp.cnp.ecnBits=3;
        for (int i=0;i<10;++i) b->ObserveFeedback(cnp);
        a->ObserveFeedback(cnp); Check(!a->m_flows.at(1).event,"source reacted to native CNP");
        cnp.cnp.fid=999; b->ObserveFeedback(cnp); Check(b->m_unmatched==1,"unmatched CNP accounting");
        Simulator::Schedule(MicroSeconds(50),[b]() {
            b->UpdateReceiver(); auto& f=b->m_flows.at(1);
            Check(f.decreases==1 && std::abs(f.rate-968750000)<1,"CNP window coalescing");
        });
        Simulator::Schedule(MilliSeconds(1),[b]() {
            double rate=b->m_flows.at(1).rate; b->UpdateReceiver();
            Check(b->m_flows.at(1).rate==rate,"idle flow recovered without transmission");
        });
        Simulator::Schedule(MilliSeconds(2),[b]() {
            Pause(b,100); b->UpdateReceiver();
            Check(b->m_groups.at(1).service==0 && b->m_flows.at(1).budget==0,"PFC did not stop service/budget");
            Pause(b,0); b->UpdateReceiver();
            Check(b->m_flows.at(1).budget>0,"PFC resume did not restore eligible service");
        });
        Simulator::Schedule(MilliSeconds(30),[a]() {
            auto& g=a->m_groups.at(1); g.target=10; a->UpdateSource();
            Check(g.target<=10+1e-9,"stale state increased rate");
        });
        Simulator::Run(); Simulator::Destroy();
        std::cout << "R3 controller regressions passed\n";
    }
};
}
int main() {
    try {ns3::DciGatewayTestCase::Run(); return 0;}
    catch (const std::exception& e) {std::cerr << e.what() << '\n'; return 1;}
}
