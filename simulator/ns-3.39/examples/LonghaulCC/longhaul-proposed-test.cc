// Focused R1 regressions. Reuse the experiment setup, but inject faults only here.
#define main LonghaulMain
#include "longhaul-convergence.cc"
#undef main
#include "ns3/pause-header.h"

namespace {
using namespace ProposedR1;
void Check(bool condition, const char *message) {
    NS_ABORT_MSG_IF(!condition, message);
}
void ModelChecks() {
    snapshot.v[StateHeader::END]=0;
    snapshot.v[StateHeader::QUEUE]=250000;
    snapshot.v[StateHeader::SERVICE]=6250000000ULL;
    for (unsigned i=1;i<=2000;++i) history.push_back({i*1e-6,9375});
    auto estimate=Reconstruct(0.002);
    Check(std::abs(estimate.first-6500000)<1,"R1 design example 17.1");
    history.clear(); snapshot.v[StateHeader::QUEUE]=0;
    snapshot.v[StateHeader::SERVICE]=1000;
    history.push_back({0.9,100});
    estimate=Reconstruct(1.0);
    Check(std::abs(estimate.first)<1e-9 && estimate.second==100,
          "idle service must not cancel a later peak");
    history.clear(); snapshot=StateHeader();
}
Ptr<Packet> Frame(StateHeader h) {
    auto p=Create<Packet>(); p->AddHeader(h);
    Ipv4Header ip; ip.SetProtocol(249); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); return p;
}
void OldReports() {
    auto before=snapshot;
    StateHeader old=before;
    old.v[StateHeader::SEQ]--;
    old.v[StateHeader::BUDGET]=0;
    auto oldRejected=rejected;
    Receive(true,Frame(old));
    Check(snapshot.v[StateHeader::SEQ]==before.v[StateHeader::SEQ] && rejected==oldRejected+1,
          "old sequence must not overwrite budget");
    old=before; old.v[StateHeader::EPOCH]=0; old.v[StateHeader::SEQ]++;
    Receive(true,Frame(old));
    Check(snapshot.v[StateHeader::SEQ]==before.v[StateHeader::SEQ] && rejected==oldRejected+2,
          "old epoch must not overwrite budget");
}
bool LoseReports(Ptr<const Packet> p) {
    CustomHeader ch(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
    p->PeekHeader(ch);
    double now=Simulator::Now().GetSeconds();
    if (ch.l3Prot==249 && now>=0.025 && now<0.03) return true;
    return Receive(true,p);
}
void InstallLoss() { dci_left_device->m_linkControlReceive=MakeCallback(&LoseReports); }
void PauseReceiver() {
    auto route=nbr2if[n.Get(research_receiver)].at(n.Get(dci_right));
    auto dev=DynamicCast<QbbNetDevice>(n.Get(research_receiver)->GetDevice(route.idx));
    auto p=Create<Packet>(); PauseHeader pause(2000,0,pg); p->AddHeader(pause);
    Ipv4Header ip; ip.SetProtocol(0xfe); ip.SetTtl(1); ip.SetPayloadSize(p->GetSize());
    p->AddHeader(ip); PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
    dev->RdmaEnqueueHighPrioQ(p); dev->TriggerTransmit();
}
void CheckEmergency() { Check(mode=="EMERGENCY" && target==0,"receiver pause must stop new injection"); }
void CheckStale() { Check(mode=="STALE","lost reports must enter STALE"); }
void CheckRecovered() { Check(mode!="STALE" && target>0,"fresh reports must recover the gate"); }
}
int main(int argc,char **argv) {
    ModelChecks();
    if (argc==1) { std::cout << "R1 history model checks passed\n"; return 0; }
    Simulator::Schedule(Seconds(0.002),&InstallLoss);
    Simulator::Schedule(Seconds(0.015),&PauseReceiver);
    Simulator::Schedule(Seconds(0.017),&CheckEmergency);
    Simulator::Schedule(Seconds(0.024),&OldReports);
    Simulator::Schedule(Seconds(0.029),&CheckStale);
    Simulator::Schedule(Seconds(0.035),&CheckRecovered);
    int result=LonghaulMain(argc,argv);
    Check(selected_cc=="proposed","integration test requires --cc=proposed");
    Check(sourceIn==sourceTx,"finite test must drain the source FIFO");
    Check(receiverIn==receiverOut,"finite test must drain the receiver FIFO");
    Check(sourceTx==receiverIn,"no data loss between the WAN measurement points");
    for (const auto &f:flows) Check(f.finished,"finite test must finish all flows");
    std::cout << "R1 pause, report loss, version, conservation and completion checks passed\n";
    return result;
}
