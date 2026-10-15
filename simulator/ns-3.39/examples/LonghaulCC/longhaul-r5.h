// Experiment wiring only. R5 group control state belongs to DciGatewayNode.
namespace Proposed {
// Algorithm defaults live in DciGatewayNode::Config, not the experiment config.
DciGatewayNode::Config config;
std::string output;
Ipv4Address ControlAddress(uint32_t node) { return DciGatewayNode::ControlAddress(node); }
void Setup() {
    if (selected_cc!="proposed") return;
    config.bufferBytes=uint64_t(buffer_size)*1024*1024;
    for (uint32_t target:{dci_gateway_a_id,dci_gateway_b_id}) {
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
        auto packet=LonghaulRouting::RoutePacket(serverAddress[f.src],serverAddress[f.dst],0x11);
        auto path=LonghaulRouting::TracePath(f.src,f.dst,packet,h);
        size_t wan=path.size();
        for (size_t j=0;j+1<path.size();++j)
            if ((path[j]==dci_gateway_a_id && path[j+1]==dci_gateway_b_id) ||
                (path[j]==dci_gateway_b_id && path[j+1]==dci_gateway_a_id)) wan=j;
        if (wan==path.size()) continue;
        NS_ABORT_MSG_IF(wan+2>=path.size(),"R5 missing B inward egress");
        uint32_t a=path[wan],b=path[wan+1];
        uint32_t aPort=nbr2if[n.Get(a)].at(n.Get(b)).idx;
        uint32_t bPort=nbr2if[n.Get(b)].at(n.Get(path[wan+2])).idx;
        auto aDev=DynamicCast<QbbNetDevice>(n.Get(a)->GetDevice(aPort));
        auto bWan=DynamicCast<QbbNetDevice>(n.Get(b)->GetDevice(nbr2if[n.Get(b)].at(n.Get(a)).idx));
        uint32_t frame=packet_payload_size+h.GetSerializedSize();
        // Connection and route association only: neither start time nor size is given to R5.
        CustomHeader back(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
        back.l3Prot=0xff;
        auto reverse=LonghaulRouting::TracePath(a,f.src,LonghaulRouting::RoutePacket(serverAddress[f.dst],serverAddress[f.src],0xff),back);
        std::vector<uint32_t> localForward(path.begin(),path.begin()+wan+1);
        DciGatewayNode::Registration r;
        r.id=i+1; r.sip=serverAddress[f.src].Get(); r.dip=serverAddress[f.dst].Get();
        r.sport=f.port; r.dport=f.dport; r.pg=f.pg; r.frameBytes=frame;
        r.propagation=DynamicCast<QbbChannel>(aDev->GetChannel())->GetDelay()+bWan->GetReceiveDelay();
        r.localResponse=Seconds(LonghaulRouting::PathDelay(reverse,64)+LonghaulRouting::PathDelay(localForward,frame))+MicroSeconds(rate_decrease_interval);
        r.role=DciGatewayNode::Role::Source; r.peer=b; r.port=aPort; r.remotePort=bPort;
        DynamicCast<DciGatewayNode>(n.Get(a))->RegisterFlow(r);
        r.role=DciGatewayNode::Role::Receiver; r.peer=a; r.port=bPort; r.remotePort=aPort;
        DynamicCast<DciGatewayNode>(n.Get(b))->RegisterFlow(r); ++registered;
        paths << i+1 << ',' << a << ',' << b << ',' << bPort << ',' << f.pg << ',';
        for (size_t j=0;j<path.size();++j) paths << (j ? "->" : "") << path[j];
        paths << '\n';
    }
    NS_ABORT_MSG_IF(!registered,"R5 requires cross-DC flows");
    for (uint32_t node:{dci_gateway_a_id,dci_gateway_b_id}) DynamicCast<DciGatewayNode>(n.Get(node))->Start();
}
void WriteMetadata(std::ostream& out) {
    out << "  \"proposed_parameters\": {\"version\":5,\"schema_version\":2,\"enabled\":" << (selected_cc=="proposed" ? "true" : "false")
        << ",\"service_mode\":\"unpaused-capacity-times-availability\",\"queue_mode\":\"packet-history\""
        << ",\"feedback_mode\":\"periodic-and-pause-change\",\"feedback_change_delay_s\":" << config.control
        << ",\"near_cnp_mode\":\"one-contributor-per-response-round-with-native-wait\""
        << ",\"group_scheduler\":\"independent-packet-round-robin\""
        << ",\"ecn_policy\":\"preserve CE and native CNP; ordinary ECN remains enabled\""
        << ",\"control_domain\":\"B egress port and PG; one managed A\""
        << ",\"path_policy\":\"fixed direct WAN; route mismatch fails closed\""
        << ",\"byte_unit\":\"complete serialized packet bytes\",\"actuation_delay_s\":0"
        << ",\"report_period_s\":" << config.period << ",\"control_period_s\":" << config.control
        << ",\"tau_s\":" << config.tau << ",\"state_timeout_s\":" << config.timeout
        << ",\"cold_fraction\":" << config.coldFraction << ",\"conservative_fraction\":" << config.conservativeFraction
        << ",\"queue_target_bytes\":" << config.qref << ",\"prediction_margin_bytes\":" << config.margin
        << ",\"source_high_bytes\":" << config.sourceHigh << ",\"source_low_bytes\":" << config.sourceLow
        << ",\"local_wait_s\":" << config.localWait << ",\"cnp_interval_s\":" << config.cnpInterval
        << ",\"local_window_s\":" << config.localWindow << ",\"buffer_bytes\":" << config.bufferBytes
        << ",\"gateways\":[";
    bool comma=false;
    if (selected_cc=="proposed") for (uint32_t node:{dci_gateway_a_id,dci_gateway_b_id}) {
        if (comma) out << ',';
        DynamicCast<DciGatewayNode>(n.Get(node))->WriteMetadata(out); comma=true;
    }
    out << "]},\n";
}
void Finish() {
    if (selected_cc!="proposed") return;
    std::ofstream out(output+".final.json"); NS_ABORT_MSG_IF(!out,"cannot write R5 final state");
    out << "{\n"; WriteMetadata(out); out << "\"time_ns\":" << Simulator::Now().GetNanoSeconds() << "}\n";
}
} // namespace Proposed
