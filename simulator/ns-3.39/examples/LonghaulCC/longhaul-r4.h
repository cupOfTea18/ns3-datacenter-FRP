// Experiment wiring only. All R4 runtime control state belongs to DciGatewayNode.
namespace Proposed {
DciGatewayNode::Config config;
std::string output;
uint32_t version=0;
bool serviceWindowSet=false,serviceMinBusySet=false;
bool ecnModeSet=false,shaperEcnSet=false;
std::vector<std::string> legacyKeys;
Ipv4Address ControlAddress(uint32_t node) { return DciGatewayNode::ControlAddress(node); }
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
void Setup() {
    if (selected_cc!="proposed") return;
    NS_ABORT_MSG_IF(version!=4,"Proposed R4 requires PROPOSED_VERSION 4; use config-longhaul-r4.txt");
    NS_ABORT_MSG_IF(!ecnModeSet,"R4 requires explicit PROPOSED_ECN_MODE source-boundary or path; migrate legacy configs");
    NS_ABORT_MSG_IF(shaperEcnSet && config.ecnMode!="path",
        "PROPOSED_SHAPER_ECN is a legacy path-mode ablation; remove it for source-boundary mode");
    NS_ABORT_MSG_IF(!legacyKeys.empty(),"R4 legacy parameter must be migrated to PORT_QREF/SOURCE_PORT_HIGH/LOW/BUFFER_FALLBACK_FRACTION");
    if (!serviceWindowSet) config.serviceWindow=config.period;
    if (!serviceMinBusySet) config.serviceMinBusy=config.control;
    config.bufferBytes=uint64_t(buffer_size)*1024*1024;
    NS_ABORT_MSG_IF(config.sourceHigh>=buffer_size*1024.0*1024,"R4 source watermark exceeds shared buffer");
    for (uint32_t target:{dci_left,dci_right}) {
        auto gateway=DynamicCast<DciGatewayNode>(n.Get(target));
        NS_ABORT_MSG_IF(!gateway,"DCI must be a DciGatewayNode"); gateway->Configure(config,output);
        CalculateRoute(n.Get(target)); auto address=ControlAddress(target);
        for (uint32_t node=0;node<n.GetN();++node) if (n.Get(node)->GetNodeType() && node!=target)
            for (auto hop:nextHop[n.Get(node)][n.Get(target)])
                DynamicCast<SwitchNode>(n.Get(node))->AddTableEntry(address,nbr2if[n.Get(node)].at(hop).idx);
    }
    uint32_t registered=0;
    std::ofstream paths; RequireOutput(paths,output+".paths.csv","flow,source_dci,destination_dci,b_egress,pg,path");
    for (uint32_t i=0;i<flows.size();++i) {
        const auto& f=flows[i];
        CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        h.l3Prot=0x11; h.udp.sport=f.port; h.udp.dport=f.dport; h.udp.pg=f.pg;
        auto packet=RoutePacket(serverAddress[f.src],serverAddress[f.dst],0x11);
        auto path=TracePath(f.src,f.dst,packet,h);
        size_t wan=path.size();
        for (size_t j=0;j+1<path.size();++j)
            if ((path[j]==dci_left && path[j+1]==dci_right) || (path[j]==dci_right && path[j+1]==dci_left)) wan=j;
        if (wan==path.size()) continue;
        NS_ABORT_MSG_IF(wan+2>=path.size(),"R4 missing inward gateway egress");
        uint32_t a=path[wan],b=path[wan+1];
        uint32_t aPort=nbr2if[n.Get(a)].at(n.Get(b)).idx;
        uint32_t bPort=nbr2if[n.Get(b)].at(n.Get(path[wan+2])).idx;
        ++registered;
        // Verify both native ACK-CNP and independent CNP traverse the B gateway.
        std::vector<uint32_t> reverse;
        for (uint8_t protocol:{uint8_t(0xfc),uint8_t(0xff)}) {
            CustomHeader back(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
            back.l3Prot=protocol; back.ack.sport=f.dport; back.ack.dport=f.port;
            auto route=TracePath(f.dst,f.src,RoutePacket(serverAddress[f.dst],serverAddress[f.src],protocol),back);
            NS_ABORT_MSG_IF(std::find(route.begin(),route.end(),b)==route.end(),"R4 CNP return bypasses receiver gateway");
            if (protocol==0xfc) reverse=route;
        }
        uint32_t burst=packet_payload_size+h.GetSerializedSize();
        std::vector<uint32_t> localForward(path.begin()+wan+1,path.end());
        auto pos=std::find(reverse.begin(),reverse.end(),b);
        std::vector<uint32_t> localReverse(reverse.begin(),pos+1);
        double localRtt=PathDelay(localForward,burst)+PathDelay(localReverse,64);
        double delay=PathDelay({a,b},burst);
        NS_ABORT_MSG_IF(config.timeout<=delay+3*config.period,"R4 timeout too short for WAN feedback");
        DciGatewayNode::Registration r;
        r.id=i+1; r.sip=serverAddress[f.src].Get(); r.dip=serverAddress[f.dst].Get();
        r.sport=f.port; r.dport=f.dport; r.pg=f.pg; r.frameBytes=burst; r.forwardDelay=Seconds(delay);
        r.feedbackRtt=Seconds(localRtt); r.start=Seconds(f.start_time);
        r.role=DciGatewayNode::Role::Source; r.peer=b;
        r.peerPortBytesPerSec=DynamicCast<QbbNetDevice>(n.Get(b)->GetDevice(bPort))->GetDataRate().GetBitRate()/8.0; r.port=aPort; DynamicCast<DciGatewayNode>(n.Get(a))->RegisterFlow(r);
        r.role=DciGatewayNode::Role::Receiver; r.peer=a;
        r.peerPortBytesPerSec=DynamicCast<QbbNetDevice>(n.Get(a)->GetDevice(aPort))->GetDataRate().GetBitRate()/8.0; r.port=bPort; DynamicCast<DciGatewayNode>(n.Get(b))->RegisterFlow(r);
        paths << i+1 << ',' << a << ',' << b << ',' << bPort << ',' << f.pg << ',';
        for (size_t j=0;j<path.size();++j) paths << (j ? "->" : "") << path[j];
        paths << '\n';
    }
    NS_ABORT_MSG_IF(!registered,"R4 requires cross-DC flows");
    for (uint32_t node:{dci_left,dci_right}) DynamicCast<DciGatewayNode>(n.Get(node))->Start();
}
void WriteMetadata(std::ostream& out) {
    out << "  \"proposed_parameters\": {\"version\":4,\"schema_version\":1,\"enabled\":" << (selected_cc=="proposed" ? "true" : "false")
        << ",\"reconstruct\":" << (config.reconstruct ? "true" : "false")
        << ",\"shaper_ecn\":" << (config.shaperEcn ? "true" : "false")
        << ",\"ecn_mode\":\"" << config.ecnMode << "\",\"startup_mode\":\"" << config.startupMode << "\""
        << ",\"ecn_policy_revision\":2"
        << ",\"source_ce_to_near_cnp\":" << (config.ecnMode=="source-boundary" ? "true" : "false")
        << ",\"source_ce_clear\":" << (config.ecnMode=="source-boundary" ? "true" : "false")
        << ",\"source_shaper_ecn\":" << (config.ecnMode=="path" && config.shaperEcn ? "true" : "false")
        << ",\"receiver_shaper_ecn\":" << (config.ecnMode=="path" && config.shaperEcn ? "true" : "false")
        << ",\"feedback_scope\":\"" << (config.ecnMode=="source-boundary" ? "source CE converted at A; A/B controlled egresses do not mark; B preserves existing CE" : "path ECN; no boundary isolation")
        << "\",\"flow_identity\":\"registered IP/UDP ports/PG, no reuse\""
        << ",\"report_period_s\":" << config.period << ",\"control_period_s\":" << config.control
        << ",\"bin_width_s\":" << config.bin << ",\"reaction_window_s\":" << config.reaction
        << ",\"gamma\":" << config.gamma << ",\"recovery_period_s\":" << config.recovery
        << ",\"probe_rate_Bps\":" << config.probeRate << ",\"rate_increase_Bps\":" << config.increase
        << ",\"probe_bytes\":" << config.probeBytes << ",\"port_qref_bytes\":" << config.qref
        << ",\"tau_s\":" << config.tau << ",\"state_timeout_s\":" << config.timeout
        << ",\"buffer_fallback_fraction\":" << config.fallback << ",\"utilization\":" << config.utilization
        << ",\"source_port_high_bytes\":" << config.sourceHigh << ",\"source_port_low_bytes\":" << config.sourceLow
        << ",\"cnp_interval_s\":" << config.cnpInterval << ",\"report_min_interval_s\":" << config.reportMin
        << ",\"service_mode\":\"" << config.serviceMode << "\",\"service_window_s\":" << config.serviceWindow
        << ",\"service_min_busy_s\":" << config.serviceMinBusy << ",\"probe_port_fraction\":" << config.probePortFraction
        << ",\"probe_port_burst_bytes\":" << config.probePortBurst << ",\"near_cnp\":" << (config.nearCnp ? "true" : "false")
        << ",\"control_ip_mtu\":" << config.controlIpMtu << ",\"gateways\":[";
    bool comma=false;
    if (selected_cc=="proposed") for (uint32_t node:{dci_left,dci_right}) {
        if (comma) out << ',';
        DynamicCast<DciGatewayNode>(n.Get(node))->WriteMetadata(out); comma=true;
    }
    out << "]},\n";
}
} // namespace Proposed
