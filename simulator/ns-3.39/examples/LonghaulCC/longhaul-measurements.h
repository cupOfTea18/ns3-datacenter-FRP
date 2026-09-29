// Common read-only experiment measurements, enabled for every algorithm.
namespace LonghaulMeasurements {
std::ofstream flowState, queues;
std::set<std::tuple<uint32_t,uint32_t,uint32_t>> observedPorts;

void WritePath(std::ostream& out,const std::vector<uint32_t>& path,uint32_t pg) {
    out << '[';
    for (size_t j=1;j<path.size();++j) {
        auto node=n.Get(path[j-1]);
        uint32_t port=nbr2if[node].at(n.Get(path[j])).idx;
        auto dev=DynamicCast<QbbNetDevice>(node->GetDevice(port));
        out << (j>1 ? "," : "") << '[' << path[j-1] << ',' << path[j] << ',' << dev->GetDataRate().GetBitRate() << ']';
        if (node->GetNodeType()) observedPorts.emplace(path[j-1],port,pg);
    }
    out << ']';
}
void WriteFlowPath(std::ostream& out,const FlowInput& f) {
    CustomHeader h(CustomHeader::L2_Header|CustomHeader::L3_Header|CustomHeader::L4_Header);
    h.l3Prot=0x11; h.udp.sport=f.port; h.udp.dport=f.dport; h.udp.pg=f.pg;
    auto path=Proposed::TracePath(f.src,f.dst,Proposed::RoutePacket(serverAddress[f.src],serverAddress[f.dst],0x11),h);
    out << ",\"data_path\":"; WritePath(out,path,f.pg);
    h.l3Prot=0xfc; h.ack.sport=f.dport; h.ack.dport=f.port;
    auto reverse=Proposed::TracePath(f.dst,f.src,Proposed::RoutePacket(serverAddress[f.dst],serverAddress[f.src],0xfc),h);
    out << ",\"ack_path\":"; WritePath(out,reverse,ack_high_prio ? 0 : f.pg);
    out << ",\"ack_wire_bytes\":" << RdmaAckWireBytes() << ",\"ack_interval_bytes\":" << l2_ack_interval;
}
void WriteResources(std::ostream& out) {
    out << "  \"buffer_resources\":["; bool first=true;
    for (uint32_t i=0;i<n.GetN();++i) if (n.Get(i)->GetNodeType()) {
        uint64_t headroom=0;
        out << (first ? "" : ",") << "{\"node\":" << i << ",\"shared_bytes\":" << uint64_t(buffer_size)*1024*1024 << ",\"ports\":[";
        bool comma=false;
        for (uint32_t j=0;j<n.Get(i)->GetNDevices();++j) if (auto d=DynamicCast<QbbNetDevice>(n.Get(i)->GetDevice(j))) {
            uint64_t h=ConfiguredHeadroomBytes(d); headroom+=8*h;
            out << (comma ? "," : "") << "{\"port\":" << j << ",\"peer\":" << Proposed::Peer(d)
                << ",\"headroom_per_pg_bytes\":" << h << ",\"pg_count\":8}"; comma=true;
        }
        out << "],\"buffer_pool_bytes\":" << uint64_t(buffer_size)*1024*1024+headroom << '}'; first=false;
    }
    out << "],\n";
}
void Sample() {
    auto now=Simulator::Now();
    for (size_t i=0;i<flows.size();++i) {
        auto& f=flows[i]; if (now.GetSeconds()<f.start_time) continue;
        auto rx=FindRxQp(f);
        if (rx) f.observed_rx=rx->m_recv_bytes;
        flowState << now.GetNanoSeconds() << ',' << i+1 << ',' << f.size_bytes << ',' << f.unique_sent
            << ',' << f.payload_sent << ',' << (rx ? rx->m_recv_bytes : f.observed_rx) << ',' << f.supply_end_ns
            << ',' << f.completion_ns << '\n';
    }
    for (const auto& entry:observedPorts) {
        uint32_t node,port,pg; std::tie(node,port,pg)=entry;
        auto d=DynamicCast<QbbNetDevice>(n.Get(node)->GetDevice(port));
        queues << now.GetNanoSeconds() << ',' << node << ',' << port << ',' << Proposed::Peer(d) << ',' << pg
            << ',' << d->GetQueue()->GetNBytes(pg) << ',' << d->IsQueuePaused(pg) << '\n';
    }
    if (now+MicroSeconds(rate_sample_interval_us)<Seconds(simulator_stop_time))
        Simulator::Schedule(MicroSeconds(rate_sample_interval_us),&Sample);
}
void StartMeasurements() {
    RequireOutput(flowState,summary_meta_file+".flow-state.csv",
        "time_ns,flow,size_bytes,unique_sent_bytes,tx_payload_bytes,rx_payload_bytes,supply_end_ns,completion_ns");
    RequireOutput(queues,summary_meta_file+".queues.csv","time_ns,node,port,peer,pg,queue_bytes,paused");
    Simulator::Schedule(MicroSeconds(rate_sample_interval_us),&Sample);
}
void Finish() {
    Sample(); // exact stop-time counters, including the final partial interval
    uint64_t drops=0,queued=0,completed=0;
    for (const auto& f:flows) completed+=f.finished;
    for (uint32_t i=0;i<n.GetN();++i) if (auto sw=DynamicCast<SwitchNode>(n.Get(i))) {
        drops+=sw->m_admissionDropPackets;
        for (uint32_t j=0;j<sw->GetNDevices();++j)
            if (auto d=DynamicCast<QbbNetDevice>(sw->GetDevice(j))) queued+=d->GetQueue()->GetNBytesTotal();
    }
    auto write=[&](const std::string& path) {
        std::ofstream out(path); NS_ABORT_MSG_IF(!out,"cannot open run summary");
        out << "{\"expected_flows\":" << flows.size() << ",\"completed_flows\":" << completed
            << ",\"admission_drop_packets\":" << drops << ",\"remaining_switch_queue_bytes\":" << queued << ",\"flows\":[";
        for (size_t i=0;i<flows.size();++i) {
            const auto& f=flows[i]; auto rx=FindRxQp(f);
            out << (i ? "," : "") << "{\"flow\":" << i+1 << ",\"size_bytes\":" << f.size_bytes
                << ",\"unique_sent_bytes\":" << f.unique_sent << ",\"tx_payload_bytes\":" << f.payload_sent
                << ",\"rx_payload_bytes\":" << (rx ? rx->m_recv_bytes : f.observed_rx) << ",\"supply_end_ns\":" << f.supply_end_ns
                << ",\"completion_ns\":" << f.completion_ns << '}';
        }
        out << "]}\n";
    };
    write(summary_meta_file+".summary.json");
    if (selected_cc=="proposed") write(Proposed::output+".summary.json");
    flowState.close(); queues.close();
}
}
