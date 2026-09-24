// Minimal receiver-side control experiment. Included by longhaul-convergence.cc.
// Receiver targets/telemetry use delayed events; near-source CNPs are real packets.
#include <iomanip>
#include "ns3/cn-header.h"
#include "ns3/ppp-header.h"
uint32_t research_receiver = 0;
uint32_t research_control = 0; // 0: baseline, 1: reactive, 2: prediction
std::string research_output;
Ptr<QbbNetDevice> research_port;
std::ofstream research_file;
uint64_t research_enqueued = 0, research_departed = 0, research_marked = 0;
uint64_t research_source_sent = 0, research_wire_sent = 0;
uint64_t research_last_enqueued = 0, research_last_departed = 0;
uint64_t research_last_source = 0, research_last_wire = 0;
std::map<uint32_t, double> research_seen;
double research_telemetry = 0, research_telemetry_time = 0;
double research_delay = 0, research_horizon = 0;
double research_period = 0.001;
double research_qref = 1000000; // bytes
double research_virtual_queue = 0, research_source_target = 0;
uint64_t research_cnp_sent = 0;
std::map<uint32_t, uint64_t> research_source_in, research_source_previous;
std::map<uint32_t, double> research_source_seen;
bool research_marking = false;
double research_near_period = 0.0001, research_deadband = 0.05;
double research_target_util = 0.98, research_forecast_weight = 0.5;
double research_increase_fraction = 0.1;
double research_last_target = 0;
uint32_t research_previous_active = 0;
bool research_guarded = false; // enabled by unified quick experiments; legacy default preserved

void ResearchSourceEnqueue(Ptr<const Packet> packet, uint32_t);

bool ResearchData(Ptr<const Packet> packet, CustomHeader &h) {
    packet->PeekHeader(h);
    return h.l3Prot == 0x11 && h.dip == serverAddress[research_receiver].Get();
}

void ResearchEnqueue(Ptr<const Packet> packet, uint32_t) {
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    if (!ResearchData(packet, h)) return;
    research_enqueued += packet->GetSize();
    for (uint32_t i = 0; i < flows.size(); ++i) {
        const auto &f = flows[i];
        if (serverAddress[f.src].Get() == h.sip && f.port == h.udp.sport &&
            f.dport == h.udp.dport && f.pg == h.udp.pg)
            research_seen[i] = Simulator::Now().GetSeconds();
    }
}

void ResearchDequeue(Ptr<const Packet> packet, uint32_t) {
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    if (!ResearchData(packet, h)) return;
    research_departed += packet->GetSize();
    if (h.GetIpv4EcnBits() != 0) ++research_marked;
}

void ResearchSource(Ptr<const Packet> packet, uint32_t) {
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    if (ResearchData(packet, h)) research_source_sent += packet->GetSize();
}

void ResearchWire(Ptr<const Packet> packet, Ptr<RdmaQueuePair>) {
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    if (ResearchData(packet, h)) research_wire_sent += packet->GetSize();
}

void ResearchTelemetry(double rate, double timestamp) {
    research_telemetry = rate;
    research_telemetry_time = timestamp;
}

void ResearchSourceEnqueue(Ptr<const Packet> packet, uint32_t) {
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    if (!ResearchData(packet, h)) return;
    for (uint32_t i = 0; i < flows.size(); ++i) {
        const auto &f = flows[i];
        if (serverAddress[f.src].Get() == h.sip && f.port == h.udp.sport && f.dport == h.udp.dport) {
            research_source_in[i] += packet->GetSize();
            research_source_seen[i] = Simulator::Now().GetSeconds();
        }
    }
}

void ResearchTarget(double target) { research_source_target = target; }

void ResearchCnp(uint32_t i) {
    const auto &f = flows[i];
    CnHeader cn(f.port, f.pg, 3, 1, 1);
    auto packet = Create<Packet>(0);
    packet->AddHeader(cn);
    Ipv4Header ip;
    ip.SetSource(serverAddress[f.dst]);
    ip.SetDestination(serverAddress[f.src]);
    ip.SetProtocol(0xff);
    ip.SetTtl(64);
    ip.SetPayloadSize(packet->GetSize());
    packet->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); packet->AddHeader(ppp);
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(h);
    const auto &route = nbr2if[n.Get(dci_left)].at(n.Get(f.src));
    auto dev = DynamicCast<QbbNetDevice>(n.Get(dci_left)->GetDevice(route.idx));
    dev->SwitchSend(0, packet, h);
    ++research_cnp_sent;
}

void ResearchNearSource() {
    const double dt = research_near_period; // 100 us, faster than the 1 ms remote controller
    double now = Simulator::Now().GetSeconds();
    uint64_t incoming = 0;
    uint32_t active = 0;
    for (auto entry : research_source_seen)
        if (now - entry.second <= (research_guarded ? std::max(3*dt, 2*research_period) : 2*research_horizon)) ++active;
    for (auto entry : research_source_in)
        incoming += entry.second - research_source_previous[entry.first];
    research_virtual_queue = std::max(0.0, research_virtual_queue + incoming - research_source_target * dt);
    if (research_guarded && active < research_previous_active)
        research_virtual_queue = std::min(research_virtual_queue, research_qref);
    research_previous_active = active;
    if (research_virtual_queue > research_qref) research_marking = true;
    if (research_virtual_queue < research_qref / 4) research_marking = false;
    for (auto entry : research_source_in) {
        double measured = (entry.second - research_source_previous[entry.first]) / dt;
        // Gate CNP by measured fair share to avoid repeatedly punishing flows
        // already below target while historical virtual backlog drains.
        if (research_marking && active && measured > (1+research_deadband) * research_source_target / active)
            ResearchCnp(entry.first);
        research_source_previous[entry.first] = entry.second;
    }
    if (now + dt <= simulator_stop_time + 1e-12)
        Simulator::Schedule(Seconds(dt), &ResearchNearSource);
}

// Minimal zero-order predictor: source egress measurements arrive after df.
// They estimate current receiver arrivals; hold that measured rate over H.
// Crucially a target is never assumed to have been executed by the RNIC.
double ResearchPredict(double, double queue, double capacity) {
    double increment = (research_telemetry - capacity) * research_horizon;
    if (research_guarded) increment *= research_forecast_weight;
    return std::max(0.0, queue + increment);
}

void ResearchSample() {
    double now = Simulator::Now().GetSeconds();
    double capacity = research_port->GetDataRate().GetBitRate() / 8.0;
    double queue = LinkQueueBytes(research_port);
    double arrival = (research_enqueued - research_last_enqueued) / research_period;
    double departure = (research_departed - research_last_departed) / research_period;
    double source = (research_source_sent - research_last_source) / research_period;
    double wire = (research_wire_sent - research_last_wire) / research_period;
    Simulator::Schedule(Seconds(research_delay), &ResearchTelemetry, source, now);
    std::vector<uint32_t> members;
    // Receiver learns membership from received packets, with a 2 RTT timeout.
    for (auto entry : research_seen)
        if (now - entry.second <= 2 * research_horizon) members.push_back(entry.first);
    double predicted = ResearchPredict(now, queue, capacity);
    double target = 0;
    if (research_control && !members.empty()) {
        double q = research_control == 2 ? predicted : queue;
        target = std::max(1e8 / 8 * members.size(), std::min(research_target_util * capacity,
                    capacity - (q - research_qref) / research_horizon));
        if (research_guarded) {
            // Stale telemetry is not evidence of current arrivals: use queue-only control.
            if (now - research_telemetry_time > research_delay + 2*research_period)
                target = std::max(1e8/8*members.size(), std::min(research_target_util*capacity,
                            capacity-(queue-research_qref)/research_horizon));
            target = std::min(target, research_last_target + research_increase_fraction*capacity);
        }
        research_last_target = target;
        Simulator::Schedule(Seconds(research_delay), &ResearchTarget, target);
    }
    uint64_t drops = 0;
    for (uint32_t i = 0; i < n.GetN(); ++i)
        if (n.Get(i)->GetNodeType()) drops += DynamicCast<SwitchNode>(n.Get(i))->m_admissionDropPackets;
    research_file << std::setprecision(12) << now << ',' << queue << ',' << arrival * 8 << ','
        << departure * 8 << ',' << wire * 8 << ',' << source * 8 << ',' << research_telemetry * 8 << ','
        << research_telemetry_time << ',' << predicted << ',' << target * 8 << ',' << members.size()
        << ',' << research_marked << ',' << research_enqueued << ',' << research_departed
        << ',' << research_virtual_queue << ',' << research_source_target * 8 << ',' << research_cnp_sent << ',' << drops << '\n';
    research_file.flush();
    research_last_enqueued = research_enqueued;
    research_last_departed = research_departed;
    research_last_source = research_source_sent;
    research_last_wire = research_wire_sent;
    if (now + research_period <= simulator_stop_time + 1e-12)
        Simulator::Schedule(Seconds(research_period), &ResearchSample);
}

void ResearchSetup() {
    if (research_output.empty()) return;
    NS_ABORT_MSG_IF(research_receiver >= n.GetN(), "research receiver out of range");
    auto it = nbr2if[n.Get(dci_right)].find(n.Get(research_receiver));
    NS_ABORT_MSG_IF(it == nbr2if[n.Get(dci_right)].end(), "research receiver must attach directly to receiver DCI");
    research_port = DynamicCast<QbbNetDevice>(n.Get(dci_right)->GetDevice(it->second.idx));
    research_delay = DynamicCast<QbbChannel>(dci_left_device->GetChannel())->GetDelay().GetSeconds();
    research_horizon = 2 * research_delay + 2 * nic_delay_ns * 1e-9 + 3e-6;
    RequireOutput(research_file, research_output,
        "time_s,queue_bytes,arrival_bps,departure_bps,sender_wire_bps,source_dci_bps,telemetry_bps,telemetry_timestamp_s,predicted_queue_bytes,target_bps,observed_flows,ecn_packets,enqueued_bytes,departed_bytes,virtual_queue_bytes,source_target_bps,cnp_sent,admission_drop_packets");
    research_port->TraceConnectWithoutContext("QbbEnqueue", MakeCallback(&ResearchEnqueue));
    research_port->TraceConnectWithoutContext("QbbDequeue", MakeCallback(&ResearchDequeue));
    dci_left_device->TraceConnectWithoutContext("QbbDequeue", MakeCallback(&ResearchSource));
    for (uint32_t i = 0; i < n.GetN(); ++i)
        if (n.Get(i)->GetNodeType() == 0)
            DynamicCast<QbbNetDevice>(n.Get(i)->GetDevice(1))->TraceConnectWithoutContext("RdmaQpDequeue", MakeCallback(&ResearchWire));
    research_source_target = research_port->GetDataRate().GetBitRate() / 8.0;
    research_last_target = research_source_target;
    if (research_control) {
        NS_ABORT_MSG_IF(cc_mode != 1, "near-source CNP executor requires DCQCN");
        for (const auto &f : flows)
            NS_ABORT_MSG_IF(nbr2if[n.Get(dci_left)].count(n.Get(f.src)) == 0 || f.dst != research_receiver,
                "research flows must connect source DCI to the selected receiver");
        dci_left_device->TraceConnectWithoutContext("QbbEnqueue", MakeCallback(&ResearchSourceEnqueue));
        Simulator::Schedule(Seconds(research_near_period), &ResearchNearSource);
    }
    Simulator::Schedule(Seconds(research_period), &ResearchSample);
}
