// Experiment wiring and runtime source registration. Never registers future data flows.
namespace Proposed {
DciGatewayNode::Config config;
std::string output;
std::ofstream endpoints;
struct PendingRegistration {
    Ptr<RdmaHw> hw;
    Ptr<RdmaQueuePair> qp;
    uint32_t gateway=0, attempts=0;
    uint64_t query=0;
    EventId event;
};
std::map<std::pair<uint32_t,uint32_t>,PendingRegistration> registrations;
Ipv4Address ControlAddress(uint32_t node) { return DciGatewayNode::ControlAddress(node); }
void Register(std::pair<uint32_t,uint32_t> key) {
    auto it=registrations.find(key); if (it==registrations.end()) return;
    auto& r=it->second;
    if (r.attempts++>=config.maxRetries) { registrations.erase(it); return; }
    R5Header h; h.type=R5Header::REGISTER;
    auto& v=h.v; v[R5Header::SIP]=r.qp->sip.Get(); v[R5Header::DIP]=r.qp->dip.Get();
    v[R5Header::QPN]=r.qp->destQpn; v[R5Header::SRC_QPN]=r.qp->localQpn;
    v[R5Header::CONNECTION]=r.qp->connectionGeneration; v[R5Header::QUERY_ID]=++r.query;
    auto packet=Create<Packet>(); packet->AddHeader(h);
    r.hw->SendControlPacket(packet,r.qp->sip,ControlAddress(r.gateway),0xf9);
    r.event=Simulator::Schedule(Seconds(config.queryRetry),&Register,key);
}
void ReceiveRegistration(Ptr<Packet> packet,const CustomHeader& ip) {
    auto copy=packet->Copy(); copy->RemoveAtStart(ip.GetSerializedSize());
    R5Header h; if (!copy->RemoveHeader(h) || h.type!=R5Header::REGISTER_ACK) return;
    auto it=registrations.find({ip.dip,uint32_t(h.v[R5Header::SRC_QPN])});
    if (it==registrations.end()) return;
    auto& r=it->second;
    if (ip.sip!=ControlAddress(r.gateway).Get() || h.v[R5Header::SESSION]!=config.session ||
        h.v[R5Header::QUERY_ID]!=r.query || h.v[R5Header::SIP]!=r.qp->sip.Get() ||
        h.v[R5Header::DIP]!=r.qp->dip.Get() || h.v[R5Header::QPN]!=r.qp->destQpn ||
        h.v[R5Header::CONNECTION]!=r.qp->connectionGeneration) return;
    h.type=R5Header::REGISTER_CONFIRM;
    h.v[R5Header::INTERVAL_NS]=MicroSeconds(r.hw->m_rateDecreaseInterval).GetNanoSeconds();
    auto confirmation=Create<Packet>(); confirmation->AddHeader(h);
    r.hw->SendControlPacket(confirmation,r.qp->sip,ControlAddress(r.gateway),0xf9);
    Simulator::Cancel(r.event); registrations.erase(it);
}
void Connected(Ptr<RdmaQueuePair> qp) {
    auto flow=FindFlow(qp);
    NS_ABORT_MSG_IF(!flow,"connected QP missing from experiment statistics");
    flow->src_qpn=qp->localQpn; flow->dest_qpn=qp->destQpn;
    auto source=ip_to_node_id(qp->sip), destination=ip_to_node_id(qp->dip);
    endpoints<<Simulator::Now().GetNanoSeconds()<<','<<source<<','<<destination<<','<<qp->sip.Get()<<','<<qp->dip.Get()<<','
        <<qp->sport<<','<<qp->dport<<','<<qp->m_pg<<','<<qp->localQpn<<','<<qp->destQpn<<','<<qp->connectionGeneration<<'\n';
    if (selected_cc!="proposed" || addressPlan.dc[source]==addressPlan.dc[destination]) return;
    auto hw=n.Get(source)->GetObject<RdmaDriver>()->m_rdma;
    // This callback runs only after actual endpoint CM messages exchanged QPNs.
    auto key=std::make_pair(qp->sip.Get(),qp->localQpn);
    NS_ABORT_MSG_IF(registrations.size()>=config.maxFlows,"source registration pending pool exhausted");
    PendingRegistration pending; pending.hw=hw; pending.qp=qp;
    pending.gateway=addressPlan.dc[source]==1 ? dci_gateway_a_id : dci_gateway_b_id;
    registrations.emplace(key,pending); Register(key);
}
void Setup() {
    RequireOutput(endpoints,summary_meta_file+".endpoints.csv",
        "ready_ns,src,dst,src_ip,dst_ip,sport,dport,pg,src_qpn,dest_qpn,connection_generation");
    for (uint32_t node=0;node<n.GetN();++node) if (!n.Get(node)->GetNodeType()) {
        auto hw=n.Get(node)->GetObject<RdmaDriver>()->m_rdma;
        hw->m_qpConnected=MakeCallback(&Connected);
        if (selected_cc=="proposed") hw->m_controlReceive=MakeCallback(&ReceiveRegistration);
    }
    if (selected_cc!="proposed") return;
    config.bufferBytes=uint64_t(buffer_size)*1024*1024;
    for (uint32_t target:{dci_gateway_a_id,dci_gateway_b_id}) {
        auto gateway=DynamicCast<DciGatewayNode>(n.Get(target));
        NS_ABORT_MSG_IF(!gateway,"DCI must be a DciGatewayNode"); gateway->Configure(config,output);
        auto peer=target==dci_gateway_a_id ? dci_gateway_b_id : dci_gateway_a_id;
        gateway->SetPeer(peer,nbr2if[n.Get(target)].at(n.Get(peer)).idx,
                         packet_payload_size+CustomHeader::GetStaticWholeHeaderSize());
        CalculateRoute(n.Get(target)); auto address=ControlAddress(target);
        for (uint32_t node=0;node<n.GetN();++node) if (node!=target) {
            for (auto hop:nextHop[n.Get(node)][n.Get(target)]) {
                auto iface=nbr2if[n.Get(node)].at(hop).idx;
                if (n.Get(node)->GetNodeType()) DynamicCast<SwitchNode>(n.Get(node))->AddTableEntry(address,iface);
                else n.Get(node)->GetObject<RdmaDriver>()->m_rdma->AddTableEntry(address,iface);
            }
        }
        // Management-known host NIC physical ceilings, not measured available shares.
        for (uint32_t node=0;node<n.GetN();++node) if (!n.Get(node)->GetNodeType()) {
            auto link=nbr2if[n.Get(node)].begin();
            gateway->SetDestinationBudget(serverAddress[node].Get(),link->second.bw/8.0);
        }
    }
    for (uint32_t target:{dci_gateway_a_id,dci_gateway_b_id})
        DynamicCast<DciGatewayNode>(n.Get(target))->Start();
}
void WriteMetadata(std::ostream& out) {
    out<<"  \"proposed_parameters\": {\"version\":5,\"schema_version\":3,\"wire_version\":7,\"enabled\":"
       <<(selected_cc=="proposed" ? "true" : "false")
       <<",\"flow_key\":\"src-ip,dst-ip,dest-qpn\",\"discovery\":\"first-data-independent\""
       <<",\"queue_mode\":\"per-flow-full-rtt\""
       <<",\"address_plan\":\"leaf-v1\",\"qpn_format\":\"modeled-bth-field-v1-24-bit\""
       <<",\"endpoint_qpn_limit\":4096,\"endpoint_connect_attempts\":8,\"endpoint_connect_retry_s\":0.02"
       <<",\"endpoint_qpn_reuse\":\"never-during-rnic-lifetime\""
       <<",\"delay_source\":\"runtime-wire-probes-and-registration-echo\""
       <<",\"prediction_horizon\":\"decision-time-plus-forward-plus-reverse\""
       <<",\"control_dispatch\":\"immediate-new-flow-otherwise-periodic-coalesced\""
       <<",\"inter_tick_policy\":\"new-flow-allocation-retain-otherwise-immediate-safety-stop\""
       <<",\"history_retention\":\"freshness-window-and-pending-validation-samples-bounded\""
       <<",\"forecast_policy\":\"immutable-until-validation-or-deadline-reject-new-on-full\""
       <<",\"service_mode\":\"queue-service-and-approximate-fifo-mixture\""
       <<",\"future_supply\":\"recent-admission-window-packetized\""
       <<",\"near_cnp_policy\":\"local-pool-hysteresis-main-contributor-response-wait\""
       <<",\"native_cnp_policy\":\"account-and-forward-unchanged\""
       <<",\"sample_boundary_policy\":\"include-same-ns-arrivals-conservatively\""
       <<",\"destination_budget_source\":\"management-nic-physical-ceiling-not-available-share\""
       <<",\"bucket_frames\":4,\"initial_bucket\":\"full\",\"zero_rate_tokens\":\"preserve\""
       <<",\"shared_startup_permission\":false,\"risk_iteration\":false"
       <<",\"deferred\":[\"D7-pending-buffer\",\"D8-cross-generation-settlement\",\"D9-full-aging-policy\"]"
       <<",\"lifecycle_policy\":\"retain-identities-reuse-empty-queues-reject-on-exhaustion\""
       <<",\"report_period_s\":"<<config.period<<",\"control_period_s\":"<<config.control
       <<",\"state_timeout_s\":"<<config.timeout<<",\"initial_delay_s\":"<<config.initialDelay
       <<",\"probe_period_s\":"<<config.probePeriod<<",\"query_retry_s\":"<<config.queryRetry
       <<",\"startup_policy\":\"physical-capacity-with-shared-budgets\",\"feedback_policy\":\"B-push-no-query\",\"conservative_fraction\":"<<config.conservativeFraction
       <<",\"queue_target_bytes\":"<<config.qref<<",\"prediction_margin_bytes\":"<<config.margin
       <<",\"source_high_bytes\":"<<config.sourceHigh<<",\"source_low_bytes\":"<<config.sourceLow
       <<",\"local_wait_s\":"<<config.localWait<<",\"cnp_interval_s\":"<<config.cnpInterval
       <<",\"local_window_s\":"<<config.localWindow<<",\"buffer_bytes\":"<<config.bufferBytes
       <<",\"max_flows\":"<<config.maxFlows<<",\"max_history_per_flow\":"<<config.maxHistoryPerFlow
       <<",\"max_history_records\":"<<config.maxHistory<<",\"max_forecasts_per_flow\":"<<config.maxForecasts
       <<",\"max_future_packets\":"<<config.maxFuturePackets<<",\"max_flow_reports_per_sample\":"<<config.maxReports
       <<",\"control_rate_fraction\":"<<config.controlFraction<<",\"control_burst_bytes\":"<<config.controlBurstBytes
       <<",\"control_queue_bytes\":"<<config.controlQueueBytes<<",\"max_control_retries\":"<<config.maxRetries<<",\"gateways\":[";
    bool comma=false;
    if (selected_cc=="proposed") for (uint32_t node:{dci_gateway_a_id,dci_gateway_b_id}) {
        if (comma) out<<',';
        DynamicCast<DciGatewayNode>(n.Get(node))->WriteMetadata(out); comma=true;
    }
    out<<"]},\n";
}
void Finish() {
    endpoints.close();
    for (auto& item:registrations) Simulator::Cancel(item.second.event);
    registrations.clear();
    if (selected_cc!="proposed") return;
    std::ofstream out(output+".final.json"); NS_ABORT_MSG_IF(!out,"cannot write R5 final state");
    out<<"{\n"; WriteMetadata(out); out<<"\"time_ns\":"<<Simulator::Now().GetNanoSeconds()<<"}\n";
}
}
