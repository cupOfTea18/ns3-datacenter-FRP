/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <time.h>
#include "ns3/core-module.h"
#include "ns3/qbb-helper.h"
#include "ns3/internet-module.h"
#include "ns3/global-route-manager.h"
#include "ns3/packet.h"
#include "ns3/error-model.h"
#include <ns3/rdma.h>
#include <ns3/rdma-client.h>
#include <ns3/rdma-client-helper.h>
#include <ns3/rdma-driver.h>
#include <ns3/switch-node.h>
#include <ns3/sim-setting.h>

using namespace ns3;
using namespace std;

NS_LOG_COMPONENT_DEFINE("LONGHAUL_CONVERGENCE");

uint32_t cc_mode = 1;
bool enable_qcn = true;
uint32_t packet_payload_size = 1000, l2_chunk_size = 4000, l2_ack_interval = 1;
double pause_time = 5, simulator_stop_time = 0.38;
std::string topology_file = "examples/LonghaulCC/topology-longhaul.txt";
std::string flow_file = "examples/LonghaulCC/flow-longhaul-s0.txt";
std::string fct_output_file = "fct.txt";
std::string pfc_output_file = "pfc.txt";
std::string sender_rate_output_file = "sender-rate.csv";
std::string receiver_goodput_output_file = "receiver-goodput.csv";
std::string link_stats_output_file = "dci-link.csv";
std::string rtt_output_file = "measured-rtt.csv";
std::string summary_meta_file = "metadata.json";
uint64_t rate_sample_interval_us = 100;
uint64_t goodput_sample_interval_us = 100;
uint64_t rtt_sample_interval_us = 100;
uint32_t rng_seed = 1;
uint64_t rng_run = 1;
uint32_t dci_left = 40, dci_right = 81;
std::string scenario_name = "UNKNOWN";

double alpha_resume_interval = 55, rp_timer = 900, ewma_gain = 1.0 / 16.0;
double timely_alpha = 0.875, timely_beta = 0.8;
uint64_t timely_tlow_ns = 5000000, timely_thigh_ns = 10000000, timely_min_rtt_ns = 10000000;
double rate_decrease_interval = 4;
uint32_t fast_recovery_times = 5;
std::string rate_ai = "50Mb/s", rate_hai = "100Mb/s", min_rate = "100Mb/s";
std::string dctcp_rate_ai = "1000Mb/s";

bool clamp_target_rate = false, l2_back_to_zero = false;
double error_rate_per_link = 0.0;
uint32_t has_win = 1;
uint32_t mi_thresh = 5;
bool var_win = false, fast_react = true;
bool multi_rate = false;
bool sample_feedback = false;
double u_target = 0.95;
uint32_t int_multi = 1;
bool rate_bound = true;

uint32_t ack_high_prio = 0;
uint32_t buffer_size = 50;

// 网卡处理延迟（纳秒）
uint64_t nic_delay_ns = 15000;  // 默认15μs = 15000ns

unordered_map<uint64_t, uint32_t> rate2kmax;
unordered_map<uint64_t, uint32_t> rate2kmin;
unordered_map<uint64_t, double> rate2pmax;

/************************************************
 * Runtime varibles
 ***********************************************/
NodeContainer n;
uint64_t nic_rate;
uint64_t maxRtt;

struct Interface {
	uint32_t idx;
	uint64_t delay;
	uint64_t bw;

	Interface() : idx(0), delay(0), bw(0) {}
};
map<Ptr<Node>, map<Ptr<Node>, Interface> > nbr2if;
// Mapping destination to next hop for each node: <node, <dest, <nexthop0, ...> > >
map<Ptr<Node>, map<Ptr<Node>, vector<Ptr<Node> > > > nextHop;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairDelay;
map<Ptr<Node>, map<Ptr<Node>, long double> > pairSerializationNsPerByte;
map<Ptr<Node>, map<Ptr<Node>, uint32_t> > pairHopCount;
map<uint32_t, map<uint32_t, uint64_t> > pairBw;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairBdp;
map<uint32_t, map<uint32_t, uint64_t> > pairBaseRtt;

std::vector<Ipv4Address> serverAddress;

std::ofstream sender_rate_file;
std::ofstream receiver_goodput_file;
std::ofstream link_stats_file;
std::ofstream rtt_file;
std::ofstream fct_file;
std::ofstream pfc_file;
std::ofstream metadata_file;

Ptr<QbbNetDevice> dci_left_device;
Ptr<QbbNetDevice> dci_right_device;
uint64_t dci_last_time_ns = 0;
uint64_t dci_left_tx_bytes = 0;
uint64_t dci_right_tx_bytes = 0;
bool dci_sample_initialized = false;
uint64_t dci_left_ecn_events = 0, dci_right_ecn_events = 0;
uint64_t pfc_pause_events = 0, pfc_resume_events = 0;

// maintain port number for each host pair
std::unordered_map<uint32_t, unordered_map<uint32_t, uint16_t> > nextPort;

struct FlowInput {
	uint64_t src, dst, pg, size_bytes, port, dport;
	double start_time;
	uint64_t last_recv_bytes;
	uint64_t last_tx_payload_bytes;
	uint64_t last_tx_wire_bytes;
	uint64_t last_tx_time_ns;
	uint64_t last_rx_time_ns;
	uint64_t last_rtt_sample_time_ns;
	bool tx_initialized;
	bool rx_initialized;
	bool rtt_initialized;
	bool finished;
};
std::vector<FlowInput> flows;

uint32_t ip_to_node_id(Ipv4Address ip);

std::string selected_cc = "dcqcn";
std::string AlgorithmName(uint32_t mode) {
	if (selected_cc == "proposed") return "proposed";
	if (mode == 1) return "dcqcn";
	if (mode == 3) return "hpcc";
	if (mode == 7) return "timely";
	if (mode == 13) return "frp";
	return "cc-mode-" + std::to_string(mode);
}

void RequireOutput(std::ofstream &file, const std::string &path, const std::string &header) {
	file.open(path.c_str(), std::ios::out | std::ios::trunc);
	if (!file.is_open()) {
		NS_FATAL_ERROR("longhaul: cannot open output file: " << path);
	}
	file << header << '\n';
}

void WriteSenderRate(const FlowInput &flow, uint16_t sport, uint64_t start_ns, uint64_t end_ns,
			 uint64_t payload_bytes, uint64_t wire_bytes) {
	if (!sender_rate_file.is_open() || end_ns <= start_ns) return;
	double interval_s = double(end_ns - start_ns) * 1e-9;
	sender_rate_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ','
		<< start_ns << ',' << end_ns << ',' << flow.src << ',' << flow.dst << ',' << sport << ','
		<< flow.dport << ',' << flow.pg << ',' << payload_bytes << ',' << wire_bytes << ','
		<< std::fixed << std::setprecision(3) << payload_bytes * 8.0 / interval_s << ','
		<< wire_bytes * 8.0 / interval_s << '\n';
}

void WriteGoodput(const FlowInput &flow, uint64_t start_ns, uint64_t end_ns, uint64_t bytes) {
	if (!receiver_goodput_file.is_open() || end_ns <= start_ns) return;
	double interval_s = double(end_ns - start_ns) * 1e-9;
	receiver_goodput_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ','
		<< start_ns << ',' << end_ns << ',' << flow.src << ',' << flow.dst << ',' << flow.port << ','
		<< flow.dport << ',' << flow.pg << ',' << bytes << ','
		<< std::fixed << std::setprecision(3) << bytes * 8.0 / interval_s << '\n';
}

void WriteRttSummary(const FlowInput &flow, uint64_t start_ns, uint64_t end_ns,
		     std::vector<uint64_t> &samples) {
	if (!rtt_file.is_open() || samples.empty() || end_ns <= start_ns) return;
	std::sort(samples.begin(), samples.end());
	long double sum = 0;
	for (uint64_t sample : samples) sum += sample;
	size_t p50 = (samples.size() - 1) / 2;
	size_t p95 = (samples.size() * 95 + 99) / 100 - 1;
	rtt_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ','
		 << start_ns << ',' << end_ns << ',' << flow.src << ',' << flow.dst << ',' << flow.port << ','
		 << flow.dport << ',' << flow.pg << ',' << samples.size() << ',' << samples.front() << ','
		 << std::fixed << std::setprecision(3) << double(sum / samples.size()) << ','
		 << samples[p50] << ',' << samples[p95] << ',' << samples.back() << '\n';
}

void LoadFlows(std::istream &input) {
	uint32_t flow_count;
	if (!(input >> flow_count))
		NS_FATAL_ERROR("longhaul: flow file is missing its flow count");

	flows.reserve(flow_count);
	for (uint32_t i = 0; i < flow_count; ++i) {
		FlowInput flow{};
		if (!(input >> flow.src >> flow.dst >> flow.pg >> flow.dport >>
		      flow.size_bytes >> flow.start_time)) {
			NS_FATAL_ERROR("longhaul: malformed flow file near flow " << i);
		}
		if (flow.src >= n.GetN() || flow.dst >= n.GetN() ||
			n.Get(flow.src)->GetNodeType() != 0 || n.Get(flow.dst)->GetNodeType() != 0) {
			NS_FATAL_ERROR("longhaul: flow " << i << " must use valid host node IDs");
		}
		uint16_t &next_port = nextPort[flow.src][flow.dst];
		if (next_port == 0)
			next_port = 10000;
		flow.port = next_port++;
		flows.push_back(flow);
	}
}

void StartFlow(uint32_t flow_index) {
	FlowInput &flow = flows.at(flow_index);
	uint64_t window = has_win
		? pairBdp[n.Get(flow.src)][n.Get(flow.dst)]
		: 0;
	NS_ABORT_MSG_IF(window > std::numeric_limits<uint32_t>::max(),
		"per-flow BDP window exceeds the RDMA window field's uint32_t range");
	uint64_t rtt = pairBaseRtt[flow.src][flow.dst];
	RdmaClientHelper clientHelper(flow.pg, serverAddress[flow.src], serverAddress[flow.dst],
		flow.port, flow.dport, flow.size_bytes, window, rtt,
		Simulator::GetMaximumSimulationTime());
	ApplicationContainer applications = clientHelper.Install(n.Get(flow.src));
	// The callback itself is scheduled at flow.start_time, so the app starts now.
	applications.Start(Seconds(0));
}

// 全局映射表：IP地址 -> 节点ID
map<uint32_t, uint32_t> ipToNodeIdMap;  // key: IP (uint32_t), value: nodeId

Ipv4Address NodeAddress(uint32_t node_id) {
	NS_ABORT_MSG_IF(node_id >= 254, "longhaul supports at most 253 node IDs");
	uint32_t dc_id = node_id <= std::min(dci_left, dci_right) ? 1 : 2;
	std::string address = "11." + std::to_string(dc_id) + ".0." + std::to_string(node_id + 1);
	return Ipv4Address(address.c_str());
}

void AssignNodeAddresses() {
	serverAddress.assign(n.GetN(), Ipv4Address("0.0.0.0"));
	for (uint32_t node_id = 0; node_id < n.GetN(); ++node_id) {
		Ipv4Address address = NodeAddress(node_id);
		ipToNodeIdMap[address.Get()] = node_id;
		Ptr<Node> node = n.Get(node_id);
		if (node->GetNodeType() == 0)
			serverAddress[node_id] = address;
		else
			DynamicCast<SwitchNode>(node)->SetSwitchRealIp(address);
	}
}

/**
 * 从IP地址提取节点ID (用于qp_finish)
 * 使用全局映射表精确查找
 */
uint32_t ip_to_node_id(Ipv4Address ip) {
    uint32_t ipValue = ip.Get();
    
    // 在映射表中查找
    auto it = ipToNodeIdMap.find(ipValue);
    if (it != ipToNodeIdMap.end()) {
        return it->second;  // 返回精确的nodeId
    }
    
	NS_FATAL_ERROR("longhaul: IP " << ip << " is not mapped to a topology node");
}

FlowInput *FindFlow(Ptr<RdmaQueuePair> q) {
	uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
	for (auto &fi : flows) {
		if (fi.src == sid && fi.dst == did && fi.port == q->sport &&
		    fi.dport == q->dport && fi.pg == q->m_pg)
			return &fi;
	}
	return NULL;
}

Ptr<RdmaRxQueuePair> FindRxQp(const FlowInput &fi) {
	if (fi.dst >= serverAddress.size() || fi.src >= serverAddress.size()) return NULL;
	Ptr<RdmaDriver> rdma = n.Get(fi.dst)->GetObject<RdmaDriver>();
	if (rdma == NULL || rdma->m_rdma == NULL) return NULL;
	return rdma->m_rdma->GetRxQp(serverAddress[fi.dst].Get(), serverAddress[fi.src].Get(),
							fi.dport, (uint16_t)fi.port, (uint16_t)fi.pg, false);
}

void FlushRttSamples(Ptr<RdmaQueuePair> q, FlowInput *fi, uint64_t end_ns, bool force) {
	if (q == NULL || fi == NULL) return;
	if (!fi->rtt_initialized) {
		fi->rtt_initialized = true;
		fi->last_rtt_sample_time_ns = q->startTime.GetNanoSeconds();
	}
	if (end_ns <= fi->last_rtt_sample_time_ns) return;
	if (!force && end_ns - fi->last_rtt_sample_time_ns < rtt_sample_interval_us * 1000) return;
	std::vector<uint64_t> samples;
	samples.reserve(q->measured_rtt_samples.size());
	for (const auto &sample : q->measured_rtt_samples)
		samples.push_back(sample.second);
	WriteRttSummary(*fi, fi->last_rtt_sample_time_ns, end_ns, samples);
	q->measured_rtt_samples.clear();
	fi->last_rtt_sample_time_ns = end_ns;
}

uint64_t RdmaDataWireHeaderBytes() {
	// PppHeader::GetStaticSize() models a 14-byte link header in packet accounting.
	return CustomHeader::GetStaticWholeHeaderSize();
}

uint64_t RdmaAckWireBytes() {
	// ReceiveUdp pads ACKs to 60 bytes; PppHeader contributes 14 modeled bytes.
	return std::max<uint64_t>(60, 14 + 20 + CustomHeader::GetAckSerializedSize());
}

void RecordCompletionSamples(Ptr<RdmaQueuePair> q, FlowInput *fi) {
	if (fi == NULL) return;
	if (!fi->tx_initialized) {
		fi->tx_initialized = true;
		fi->last_tx_payload_bytes = 0;
		fi->last_tx_wire_bytes = 0;
		fi->last_tx_time_ns = q->startTime.GetNanoSeconds();
	}
	uint64_t tx_end_ns = q->last_tx_time_ns;
	if (fi->tx_initialized && tx_end_ns > fi->last_tx_time_ns &&
	    q->tx_payload_bytes >= fi->last_tx_payload_bytes && q->tx_wire_bytes >= fi->last_tx_wire_bytes) {
		WriteSenderRate(*fi, q->sport, fi->last_tx_time_ns, tx_end_ns,
			q->tx_payload_bytes - fi->last_tx_payload_bytes,
			q->tx_wire_bytes - fi->last_tx_wire_bytes);
	}
	Ptr<RdmaRxQueuePair> rxQp = FindRxQp(*fi);
	if (rxQp != NULL && !fi->rx_initialized) {
		fi->rx_initialized = true;
		fi->last_recv_bytes = 0;
		fi->last_rx_time_ns = Seconds(fi->start_time).GetNanoSeconds();
	}
	uint64_t rx_end_ns = rxQp != NULL ? rxQp->last_payload_rx_time_ns : 0;
	if (rxQp != NULL && fi->rx_initialized && rx_end_ns > fi->last_rx_time_ns &&
	    rxQp->m_recv_bytes >= fi->last_recv_bytes) {
		WriteGoodput(*fi, fi->last_rx_time_ns, rx_end_ns,
			rxQp->m_recv_bytes - fi->last_recv_bytes);
	}
	FlushRttSamples(q, fi, Simulator::Now().GetNanoSeconds(), true);
	fi->finished = true;
}

void qp_finish(Ptr<RdmaQueuePair> q) {
	uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
	uint64_t base_rtt = pairBaseRtt[sid][did], b = pairBw[sid][did];
	NS_ABORT_MSG_IF(packet_payload_size == 0, "PACKET_PAYLOAD_SIZE must be positive");
	uint64_t packet_count = q->m_size == 0 ? 0 : (q->m_size - 1) / packet_payload_size + 1;
	uint64_t data_header_bytes = RdmaDataWireHeaderBytes();
	uint64_t total_wire_bytes = q->m_size + packet_count * data_header_bytes;
	uint64_t first_payload_bytes = std::min<uint64_t>(q->m_size, packet_payload_size);
	uint64_t first_packet_bytes = first_payload_bytes + data_header_bytes;
	uint64_t ack_wire_bytes = RdmaAckWireBytes();
	long double serialization_factor = pairSerializationNsPerByte[n.Get(sid)][n.Get(did)];
	uint64_t first_data_ns = static_cast<uint64_t>(std::ceil(first_packet_bytes * serialization_factor));
	uint64_t ack_path_ns = static_cast<uint64_t>(std::ceil(ack_wire_bytes * serialization_factor));
	uint64_t remaining_wire_bytes = total_wire_bytes - first_packet_bytes;
	__uint128_t remaining_bits_ns = static_cast<__uint128_t>(remaining_wire_bytes) * 8000000000ULL;
	uint64_t remaining_data_ns = b == 0 ? 0 : static_cast<uint64_t>((remaining_bits_ns + b - 1) / b);
	uint64_t standalone_fct = base_rtt + first_data_ns + remaining_data_ns + ack_path_ns;
	FlowInput *fi = FindFlow(q);
	RecordCompletionSamples(q, fi);
	fct_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ','
			  << scenario_name << ',' << sid << ',' << did << ',' << q->sport << ',' << q->dport
			  << ',' << q->m_pg << ',' << q->m_size << ',' << q->startTime.GetNanoSeconds()
			  << ',' << (Simulator::Now() - q->startTime).GetNanoSeconds() << ',' << base_rtt
			  << ',' << pairHopCount[n.Get(sid)][n.Get(did)] << ',' << b
			  << ',' << pairBdp[n.Get(sid)][n.Get(did)] << ','
			  << (has_win ? pairBdp[n.Get(sid)][n.Get(did)] : 0) << ',' << standalone_fct << '\n';
	fct_file.flush();

	// remove rxQp from the receiver
	Ptr<Node> dstNode = n.Get(did);
	Ptr<RdmaDriver> rdma = dstNode->GetObject<RdmaDriver> ();
	if (rdma != NULL && rdma->m_rdma != NULL)
		rdma->m_rdma->DeleteRxQp(q->sip.Get(), q->m_pg, q->sport);
}

void get_pfc(std::ofstream *fout, Ptr<QbbNetDevice> dev, uint32_t type) {
	if (type == 1 || type == 2) pfc_pause_events++;
	if (type == 0 || type == 3) pfc_resume_events++;
	(*fout) << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ','
			<< Simulator::Now().GetNanoSeconds() << ',' << dev->GetNode()->GetId() << ','
			<< dev->GetNode()->GetNodeType() << ',' << dev->GetIfIndex() << ',' << type << '\n';
}

void CountDciEcn(Ptr<QbbNetDevice> dev, Ptr<const Packet> packet, uint32_t) {
	Ptr<Packet> copy = packet->Copy();
	CustomHeader header(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
	copy->PeekHeader(header);
	if (header.l3Prot == 0x11 && header.GetIpv4EcnBits() != 0) {
		if (dev == dci_left_device) dci_left_ecn_events++;
		if (dev == dci_right_device) dci_right_ecn_events++;
	}
}

void CalculateRoute(Ptr<Node> host) {
	// queue for the BFS.
	vector<Ptr<Node> > q;
	// Distance from the host to each node.
	map<Ptr<Node>, int> dis;
	map<Ptr<Node>, uint64_t> delay;
	map<Ptr<Node>, long double> serializationNsPerByte;
	map<Ptr<Node>, uint64_t> bw;
	// init BFS.
	q.push_back(host);
	dis[host] = 0;
	delay[host] = 0;
	serializationNsPerByte[host] = 0;
	bw[host] = 0xfffffffffffffffflu;
	// BFS.
	for (int i = 0; i < (int)q.size(); i++) {
		Ptr<Node> now = q[i];
		int d = dis[now];
		for (auto it = nbr2if[now].begin(); it != nbr2if[now].end(); it++) {
			Ptr<Node> next = it->first;
			// If 'next' have not been visited.
				if (dis.find(next) == dis.end()) {
					dis[next] = d + 1;
					delay[next] = delay[now] + it->second.delay;
					serializationNsPerByte[next] = serializationNsPerByte[now] + 8000000000.0L / it->second.bw;
					bw[next] = std::min(bw[now], it->second.bw);
				// we only enqueue switch, because we do not want packets to go through host as middle point
				if (next->GetNodeType())
					q.push_back(next);
			}
			// if 'now' is on the shortest path from 'next' to 'host'.
			if (d + 1 == dis[next]) {
				nextHop[next][host].push_back(now);
			}
		}
	}
	for (auto it : delay)
		pairDelay[it.first][host] = it.second;
	for (auto it : serializationNsPerByte)
		pairSerializationNsPerByte[it.first][host] = it.second;
	for (auto it : dis)
		pairHopCount[it.first][host] = static_cast<uint32_t>(it.second);
	for (auto it : bw)
		pairBw[it.first->GetId()][host->GetId()] = it.second;
}

void CalculateRoutes(NodeContainer &n) {
	for (int i = 0; i < (int)n.GetN(); i++) {
		Ptr<Node> node = n.Get(i);
		if (node->GetNodeType() == 0)
			CalculateRoute(node);
	}
}

void SetRoutingEntries() {
	// For each node.
	for (auto i = nextHop.begin(); i != nextHop.end(); i++) {
		Ptr<Node> node = i->first;
		auto &table = i->second;
		for (auto j = table.begin(); j != table.end(); j++) {
			// The destination node.
			Ptr<Node> dst = j->first;
			// The IP address of the dst.
			// Hosts use the stable node-wide address assigned before link creation;
			// switches use the first address on their routing interface.
			uint32_t dstNodeId = dst->GetId();
			Ipv4Address dstAddr;
			if (dst->GetNodeType() == 0 && dstNodeId < serverAddress.size()) {
				dstAddr = serverAddress[dstNodeId];
			} else {
				dstAddr = dst->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
			}
			// The next hops towards the dst.
			vector<Ptr<Node> > nexts = j->second;
			for (int k = 0; k < (int)nexts.size(); k++) {
				Ptr<Node> next = nexts[k];
				uint32_t interface = nbr2if[node][next].idx;
				if (node->GetNodeType())
					DynamicCast<SwitchNode>(node)->AddTableEntry(dstAddr, interface);
				else {
					node->GetObject<RdmaDriver>()->m_rdma->AddTableEntry(dstAddr, interface);
				}
			}
		}
	}
}

uint64_t GetNicRate() {
	for (uint32_t i = 0; i < n.GetN(); i++)
		if (n.Get(i)->GetNodeType() == 0)
			return DynamicCast<QbbNetDevice>(n.Get(i)->GetDevice(1))->GetDataRate().GetBitRate();
	NS_FATAL_ERROR("longhaul: topology contains no host node");
	return 0;
}


void SampleFlowRates() {
	uint64_t now_ns = Simulator::Now().GetNanoSeconds();
	uint64_t start_interval_ns = std::min(rate_sample_interval_us,
						std::min(goodput_sample_interval_us, rtt_sample_interval_us)) * 1000;
	for (auto &fi : flows) {
		if (fi.finished || fi.port == 0 || fi.src >= serverAddress.size() || fi.dst >= serverAddress.size())
			continue;

		Ptr<RdmaDriver> srcRdma = n.Get(fi.src)->GetObject<RdmaDriver>();
		Ptr<RdmaQueuePair> txQp = srcRdma && srcRdma->m_rdma
			? srcRdma->m_rdma->GetQp(serverAddress[fi.dst].Get(), (uint16_t)fi.port, (uint16_t)fi.pg)
			: NULL;
		if (txQp != NULL) {
			if (!fi.tx_initialized) {
				fi.tx_initialized = true;
				fi.last_tx_payload_bytes = 0;
				fi.last_tx_wire_bytes = 0;
				fi.last_tx_time_ns = txQp->startTime.GetNanoSeconds();
			}
			if (now_ns > fi.last_tx_time_ns &&
			    now_ns - fi.last_tx_time_ns >= rate_sample_interval_us * 1000) {
				WriteSenderRate(fi, txQp->sport, fi.last_tx_time_ns, now_ns,
					txQp->tx_payload_bytes - fi.last_tx_payload_bytes,
					txQp->tx_wire_bytes - fi.last_tx_wire_bytes);
				fi.last_tx_payload_bytes = txQp->tx_payload_bytes;
				fi.last_tx_wire_bytes = txQp->tx_wire_bytes;
				fi.last_tx_time_ns = now_ns;
			}
			FlushRttSamples(txQp, &fi, now_ns, false);
		}

		Ptr<RdmaRxQueuePair> rxQp = FindRxQp(fi);
		uint64_t flow_start_ns = Seconds(fi.start_time).GetNanoSeconds();
		if (now_ns >= flow_start_ns) {
			if (!fi.rx_initialized) {
				fi.rx_initialized = true;
				fi.last_recv_bytes = 0;
				fi.last_rx_time_ns = flow_start_ns;
			}
			uint64_t recv_bytes = rxQp != NULL ? rxQp->m_recv_bytes : 0;
			if (now_ns > fi.last_rx_time_ns &&
			    now_ns - fi.last_rx_time_ns >= goodput_sample_interval_us * 1000 &&
			    recv_bytes >= fi.last_recv_bytes) {
				WriteGoodput(fi, fi.last_rx_time_ns, now_ns, recv_bytes - fi.last_recv_bytes);
				fi.last_recv_bytes = recv_bytes;
				fi.last_rx_time_ns = now_ns;
			}
		}
	}
	if (Simulator::Now() + NanoSeconds(start_interval_ns) <= Seconds(simulator_stop_time))
		Simulator::Schedule(NanoSeconds(start_interval_ns), &SampleFlowRates);
}

uint64_t LinkTxCounter(Ptr<QbbNetDevice> dev) {
	return dev == NULL ? 0 : dev->totalBytesSent;
}

uint64_t LinkQueueBytes(Ptr<QbbNetDevice> dev) {
	if (dev == NULL || dev->GetQueue() == NULL) return 0;
	return dev->GetQueue()->GetNBytesTotal();
}

void SampleDciLink() {
	uint64_t now_ns = Simulator::Now().GetNanoSeconds();
	if (dci_left_device != NULL && dci_right_device != NULL) {
		uint64_t left_tx = LinkTxCounter(dci_left_device);
		uint64_t right_tx = LinkTxCounter(dci_right_device);
		if (!dci_sample_initialized) {
			dci_left_tx_bytes = left_tx;
			dci_right_tx_bytes = right_tx;
			dci_last_time_ns = now_ns;
			dci_sample_initialized = true;
		} else if (now_ns > dci_last_time_ns) {
		double dt = double(now_ns - dci_last_time_ns) * 1e-9;
		link_stats_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ',' << now_ns << ','
						<< dci_left << ',' << dci_right << ',' << "left-to-right" << ','
						<< ((left_tx - dci_left_tx_bytes) * 8.0 / dt) << ','
						<< LinkQueueBytes(dci_left_device) << ','
						<< dci_left_ecn_events << ',' << pfc_pause_events << ',' << pfc_resume_events << '\n';
		link_stats_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ',' << now_ns << ','
						<< dci_right << ',' << dci_left << ',' << "right-to-left" << ','
						<< ((right_tx - dci_right_tx_bytes) * 8.0 / dt) << ','
						<< LinkQueueBytes(dci_right_device) << ','
						<< dci_right_ecn_events << ',' << pfc_pause_events << ',' << pfc_resume_events << '\n';
		link_stats_file.flush();
		dci_left_tx_bytes = left_tx;
		dci_right_tx_bytes = right_tx;
		dci_last_time_ns = now_ns;
		}
	}
	if (Simulator::Now() + MicroSeconds(rate_sample_interval_us) <= Seconds(simulator_stop_time))
		Simulator::Schedule(MicroSeconds(rate_sample_interval_us), &SampleDciLink);
}

#include "longhaul-research.h"

void WriteMetadata(uint32_t node_num, uint32_t switch_num, uint32_t link_num) {
	uint64_t dci_rate = dci_left_device->GetDataRate().GetBitRate();
	uint64_t dci_delay = DynamicCast<QbbChannel>(dci_left_device->GetChannel())->GetDelay().GetTimeStep();
	metadata_file << "{\n"
		<< "  \"program\": \"longhaul-convergence\",\n"
		<< "  \"algorithm\": \"" << AlgorithmName(cc_mode) << "\",\n"
		<< "  \"cc_mode\": " << cc_mode << ",\n"
		<< "  \"research_control\": " << research_control << ",\n"
		<< "  \"proposed_parameters\": {\"guarded\": " << (research_guarded ? "true" : "false")
		<< ", \"period_s\": " << research_period << ", \"near_period_s\": " << research_near_period
		<< ", \"qref_bytes\": " << research_qref << ", \"forecast_weight\": " << research_forecast_weight
		<< ", \"horizon_s\": " << research_horizon << ", \"target_util\": " << research_target_util
		<< ", \"deadband\": " << research_deadband << ", \"increase_fraction\": " << research_increase_fraction << "},\n"
		<< "  \"scenario\": \"" << scenario_name << "\",\n"
		<< "  \"rng_seed\": " << rng_seed << ",\n"
		<< "  \"rng_run\": " << rng_run << ",\n"
		<< "  \"node_count\": " << node_num << ",\n"
		<< "  \"switch_count\": " << switch_num << ",\n"
		<< "  \"link_count\": " << link_num << ",\n"
		<< "  \"dci_left\": " << dci_left << ",\n"
		<< "  \"dci_right\": " << dci_right << ",\n"
		<< "  \"dci_rate_bps\": " << dci_rate << ",\n"
		<< "  \"nic_rate_bps\": " << nic_rate << ",\n"
		<< "  \"dci_delay_ns\": " << dci_delay << ",\n"
		<< "  \"nic_delay_ns\": " << nic_delay_ns << ",\n"
		<< "  \"packet_payload_size\": " << packet_payload_size << ",\n"
		<< "  \"data_header_bytes\": " << RdmaDataWireHeaderBytes() << ",\n"
		<< "  \"sender_rate_definition\": \"per-flow QP payload and packet bytes counted when each data packet begins QbbNetDevice transmission; payload includes retransmissions; tx_wire_bytes includes active protocol and INT headers\",\n"
		<< "  \"receiver_goodput_definition\": \"unique in-order RDMA payload accepted by the receiver; excludes protocol headers, control packets, and duplicate payload\",\n"
		<< "  \"dci_tx_definition\": \"per-direction DCI device totalBytesSent delta divided by the sampling interval; includes packet and control bytes\",\n"
		<< "  \"base_rtt_definition\": \"2 * one-way channel propagation + 2 * path hop count * QbbNetDevice receive processing delay; excludes serialization and queueing\",\n"
		<< "  \"window_definition\": \"when HAS_WIN is enabled, per-flow path bottleneck rate * that flow base RTT / 8; VAR_WIN may scale this cap by current rate; window_bytes is 0 when no cap is enabled\",\n"
		<< "  \"standalone_fct_definition\": \"base RTT + first data packet serialization across the forward path + remaining data wire bytes at path bottleneck + final ACK serialization across the reverse path; wire sizes use PPP, IPv4, transport, and active INT headers; no queueing, loss, or competing flows\",\n"
		<< "  \"measured_rtt_definition\": \"sender NIC packet transmit start to newly advancing cumulative ACK; retransmission-ambiguous samples excluded\",\n"
		<< "  \"rate_sample_interval_us\": " << rate_sample_interval_us << ",\n"
		<< "  \"goodput_sample_interval_us\": " << goodput_sample_interval_us << ",\n"
		<< "  \"rtt_sample_interval_us\": " << rtt_sample_interval_us << ",\n"
		<< "  \"rtt_output_file\": \"" << rtt_output_file << "\",\n"
		<< "  \"algorithm_parameters\": {\"dcqcn_ewma_gain\": " << ewma_gain
		<< ", \"hpcc_target_util\": " << u_target
		<< ", \"hpcc_mi_thresh\": " << mi_thresh
		<< ", \"timely_alpha\": " << timely_alpha
		<< ", \"timely_beta\": " << timely_beta
		<< ", \"timely_tlow_ns\": " << timely_tlow_ns
		<< ", \"timely_thigh_ns\": " << timely_thigh_ns
		<< ", \"timely_min_rtt_ns\": " << timely_min_rtt_ns << "},\n"
		<< "  \"simulator_stop_time_s\": " << simulator_stop_time << ",\n"
		<< "  \"topology_file\": \"" << topology_file << "\",\n"
		<< "  \"flow_file\": \"" << flow_file << "\",\n"
		<< "  \"flow_path_metrics\": [\n";
	for (size_t i = 0; i < flows.size(); ++i) {
		const FlowInput &flow = flows[i];
		uint64_t flow_base_rtt_ns = pairBaseRtt[flow.src][flow.dst];
		uint64_t flow_bdp_bytes = pairBdp[n.Get(flow.src)][n.Get(flow.dst)];
		metadata_file << "    {\"src\": " << flow.src << ", \"dst\": " << flow.dst
			<< ", \"sport\": " << flow.port << ", \"dport\": " << flow.dport
			<< ", \"pg\": " << flow.pg << ", \"size_bytes\": " << flow.size_bytes
			<< ", \"start_time_s\": " << flow.start_time
			<< ", \"base_rtt_ns\": " << flow_base_rtt_ns
			<< ", \"path_hops\": " << pairHopCount[n.Get(flow.src)][n.Get(flow.dst)]
			<< ", \"bottleneck_rate_bps\": " << pairBw[flow.src][flow.dst]
			<< ", \"bdp_bytes\": " << flow_bdp_bytes
			<< ", \"window_bytes\": " << (has_win ? flow_bdp_bytes : 0) << "}"
			<< (i + 1 < flows.size() ? "," : "") << '\n';
	}
	metadata_file << "  ]\n}\n";
	metadata_file.flush();
}

template <typename T>
void ReadConfigValue(std::istream &config, const std::string &key, T &value) {
	if (!(config >> value))
		NS_FATAL_ERROR("longhaul: malformed value for config key: " << key);
}

void ReadKMap(std::istream &config, const std::string &key,
		      std::unordered_map<uint64_t, uint32_t> &values) {
	uint32_t count;
	ReadConfigValue(config, key, count);
	values.clear();
	for (uint32_t i = 0; i < count; ++i) {
		uint64_t rate;
		uint32_t value;
		ReadConfigValue(config, key, rate);
		ReadConfigValue(config, key, value);
		values[rate] = value;
	}
}

void ReadPmaxMap(std::istream &config, const std::string &key) {
	uint32_t count;
	ReadConfigValue(config, key, count);
	rate2pmax.clear();
	for (uint32_t i = 0; i < count; ++i) {
		uint64_t rate;
		double value;
		ReadConfigValue(config, key, rate);
		ReadConfigValue(config, key, value);
		rate2pmax[rate] = value;
	}
}

void ParseConfig(std::istream &config) {
	std::string key;
	while (config >> key) {
		if (key == "ENABLE_QCN") ReadConfigValue(config, key, enable_qcn);
		else if (key == "PAUSE_TIME") ReadConfigValue(config, key, pause_time);
		else if (key == "CLAMP_TARGET_RATE") ReadConfigValue(config, key, clamp_target_rate);
		else if (key == "PACKET_PAYLOAD_SIZE") ReadConfigValue(config, key, packet_payload_size);
		else if (key == "L2_CHUNK_SIZE") ReadConfigValue(config, key, l2_chunk_size);
		else if (key == "L2_ACK_INTERVAL") ReadConfigValue(config, key, l2_ack_interval);
		else if (key == "L2_BACK_TO_ZERO") ReadConfigValue(config, key, l2_back_to_zero);
		else if (key == "TOPOLOGY_FILE") ReadConfigValue(config, key, topology_file);
		else if (key == "FLOW_FILE") ReadConfigValue(config, key, flow_file);
		else if (key == "SIMULATOR_STOP_TIME") ReadConfigValue(config, key, simulator_stop_time);
		else if (key == "ALPHA_RESUME_INTERVAL") ReadConfigValue(config, key, alpha_resume_interval);
		else if (key == "RP_TIMER") ReadConfigValue(config, key, rp_timer);
		else if (key == "EWMA_GAIN") ReadConfigValue(config, key, ewma_gain);
		else if (key == "FAST_RECOVERY_TIMES") ReadConfigValue(config, key, fast_recovery_times);
		else if (key == "RATE_AI") ReadConfigValue(config, key, rate_ai);
		else if (key == "RATE_HAI") ReadConfigValue(config, key, rate_hai);
		else if (key == "MIN_RATE") ReadConfigValue(config, key, min_rate);
		else if (key == "DCTCP_RATE_AI") ReadConfigValue(config, key, dctcp_rate_ai);
		else if (key == "ERROR_RATE_PER_LINK") ReadConfigValue(config, key, error_rate_per_link);
		else if (key == "CC") ReadConfigValue(config, key, selected_cc);
		else if (key == "RNG_SEED") ReadConfigValue(config, key, rng_seed);
		else if (key == "RNG_RUN") ReadConfigValue(config, key, rng_run);
		else if (key == "RATE_SAMPLE_INTERVAL_US") ReadConfigValue(config, key, rate_sample_interval_us);
		else if (key == "GOODPUT_SAMPLE_INTERVAL_US") ReadConfigValue(config, key, goodput_sample_interval_us);
		else if (key == "RTT_SAMPLE_INTERVAL_US") ReadConfigValue(config, key, rtt_sample_interval_us);
		else if (key == "RATE_OUTPUT_FILE") ReadConfigValue(config, key, sender_rate_output_file);
		else if (key == "GOODPUT_OUTPUT_FILE") ReadConfigValue(config, key, receiver_goodput_output_file);
		else if (key == "LINK_STATS_OUTPUT_FILE") ReadConfigValue(config, key, link_stats_output_file);
		else if (key == "RTT_OUTPUT_FILE") ReadConfigValue(config, key, rtt_output_file);
		else if (key == "SUMMARY_META_FILE") ReadConfigValue(config, key, summary_meta_file);
		else if (key == "DCI_LEFT") ReadConfigValue(config, key, dci_left);
		else if (key == "DCI_RIGHT") ReadConfigValue(config, key, dci_right);
		else if (key == "TIMELY_ALPHA") ReadConfigValue(config, key, timely_alpha);
		else if (key == "TIMELY_BETA") ReadConfigValue(config, key, timely_beta);
		else if (key == "TIMELY_TLOW_NS") ReadConfigValue(config, key, timely_tlow_ns);
		else if (key == "TIMELY_THIGH_NS") ReadConfigValue(config, key, timely_thigh_ns);
		else if (key == "TIMELY_MIN_RTT_NS") ReadConfigValue(config, key, timely_min_rtt_ns);
		else if (key == "RATE_DECREASE_INTERVAL") ReadConfigValue(config, key, rate_decrease_interval);
		else if (key == "FCT_OUTPUT_FILE") ReadConfigValue(config, key, fct_output_file);
		else if (key == "PFC_OUTPUT_FILE") ReadConfigValue(config, key, pfc_output_file);
		else if (key == "HAS_WIN") ReadConfigValue(config, key, has_win);
		else if (key == "MI_THRESH") ReadConfigValue(config, key, mi_thresh);
		else if (key == "VAR_WIN") ReadConfigValue(config, key, var_win);
		else if (key == "FAST_REACT") ReadConfigValue(config, key, fast_react);
		else if (key == "U_TARGET") ReadConfigValue(config, key, u_target);
		else if (key == "INT_MULTI") ReadConfigValue(config, key, int_multi);
		else if (key == "RATE_BOUND") ReadConfigValue(config, key, rate_bound);
		else if (key == "ACK_HIGH_PRIO") ReadConfigValue(config, key, ack_high_prio);
		else if (key == "KMAX_MAP") ReadKMap(config, key, rate2kmax);
		else if (key == "KMIN_MAP") ReadKMap(config, key, rate2kmin);
		else if (key == "PMAX_MAP") ReadPmaxMap(config, key);
		else if (key == "BUFFER_SIZE") ReadConfigValue(config, key, buffer_size);
		else if (key == "MULTI_RATE") ReadConfigValue(config, key, multi_rate);
		else if (key == "SAMPLE_FEEDBACK") ReadConfigValue(config, key, sample_feedback);
		else if (key == "NIC_DELAY") ReadConfigValue(config, key, nic_delay_ns);
		else if (key == "RESEARCH_RECEIVER") ReadConfigValue(config, key, research_receiver);
		else if (key == "RESEARCH_CONTROL") ReadConfigValue(config, key, research_control);
		else if (key == "RESEARCH_OUTPUT") ReadConfigValue(config, key, research_output);
		else if (key == "RESEARCH_PERIOD") ReadConfigValue(config, key, research_period);
		else if (key == "RESEARCH_NEAR_PERIOD") ReadConfigValue(config, key, research_near_period);
		else if (key == "RESEARCH_QREF") ReadConfigValue(config, key, research_qref);
		else if (key == "RESEARCH_FORECAST_WEIGHT") ReadConfigValue(config, key, research_forecast_weight);
		else if (key == "RESEARCH_DEADBAND") ReadConfigValue(config, key, research_deadband);
		else if (key == "RESEARCH_TARGET_UTIL") ReadConfigValue(config, key, research_target_util);
		else if (key == "RESEARCH_INCREASE_FRACTION") ReadConfigValue(config, key, research_increase_fraction);
		else if (key == "RESEARCH_GUARDED") ReadConfigValue(config, key, research_guarded);
		else NS_FATAL_ERROR("longhaul: unknown config key: " << key);
	}
}

std::string ScenarioFromFlowFile(const std::string &path) {
	std::string name = path.substr(path.find_last_of("/\\") + 1);
	const std::string prefix = "flow-longhaul-";
	const std::string suffix = ".txt";
	if (name.compare(0, prefix.size(), prefix) == 0)
		name = name.substr(prefix.size());
	if (name.size() >= suffix.size() &&
		name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
		name.erase(name.size() - suffix.size());
	if (name.empty()) return "UNKNOWN";
	std::transform(name.begin(), name.end(), name.begin(),
		[](unsigned char value) { return static_cast<char>(std::toupper(value)); });
	return name;
}

uint32_t ResolveCcMode(const std::string &name) {
	if (name == "dcqcn") return 1;
	if (name == "hpcc") return 3;
	if (name == "timely") return 7;
	if (name == "frp") return 13;
	if (name == "proposed") return 1;
	NS_FATAL_ERROR("longhaul: unknown congestion-control algorithm: " << name);
	return 0;
}

int main(int argc, char *argv[])
{
	clock_t begint, endt;
	begint = clock();
	std::ifstream conf;
	std::string confFile = "examples/LonghaulCC/config-longhaul-common.txt";
	CommandLine cmd;
	cmd.AddValue("cc", "dcqcn, hpcc, timely, frp, or proposed", selected_cc);
	cmd.AddValue("conf", "config file path", confFile);
	// Experiment selection: ordinary model parameters stay in the config file.
	cmd.AddValue("flow-file", "flow input path", flow_file);
	cmd.AddValue("stop-time", "simulation stop time in seconds", simulator_stop_time);
	cmd.AddValue("seed", "ns-3 RNG seed", rng_seed);
	cmd.AddValue("run", "ns-3 RNG run", rng_run);

	cmd.Parse (argc, argv);
	conf.open(confFile.c_str());
	if (!conf.is_open())
		NS_FATAL_ERROR("longhaul: cannot open config file: " << confFile);
	ParseConfig(conf);
	conf.close();
	// Re-parse so only explicitly supplied command-line values override the
	// values loaded from the configuration file.
	cmd.Parse(argc, argv);

	scenario_name = ScenarioFromFlowFile(flow_file);
	cc_mode = ResolveCcMode(selected_cc);
	if (selected_cc == "proposed" && research_output.empty())
		research_output = summary_meta_file + ".control.csv";
	NS_ABORT_MSG_IF(packet_payload_size == 0, "PACKET_PAYLOAD_SIZE must be positive");
	NS_ABORT_MSG_IF(rate_sample_interval_us == 0 || goodput_sample_interval_us == 0 ||
		rtt_sample_interval_us == 0, "rate, goodput, and RTT sample intervals must be positive");
	// Longhaul writes measurements to CSV; keep historical model diagnostics out
	// of the run log unless another program explicitly leaves this disabled.
	g_longhaul_quiet = true;
	RngSeedManager::SetSeed(rng_seed);
	RngSeedManager::SetRun(rng_run);
	RequireOutput(sender_rate_file, sender_rate_output_file,
			"algorithm,seed,run,interval_start_ns,time_ns,src,dst,sport,dport,pg,tx_payload_bytes,tx_wire_bytes,tx_payload_bps,tx_wire_bps");
	RequireOutput(receiver_goodput_file, receiver_goodput_output_file,
			"algorithm,seed,run,interval_start_ns,time_ns,src,dst,sport,dport,pg,goodput_bytes,goodput_bps");
	RequireOutput(link_stats_file, link_stats_output_file,
			"algorithm,seed,run,time_ns,src,dst,direction,tx_bps,queue_bytes,ecn_events,pfc_pause_events,pfc_resume_events");
	RequireOutput(rtt_file, rtt_output_file,
			"algorithm,seed,run,interval_start_ns,time_ns,src,dst,sport,dport,pg,sample_count,rtt_min_ns,rtt_mean_ns,rtt_p50_ns,rtt_p95_ns,rtt_max_ns");
	RequireOutput(fct_file, fct_output_file,
		"algorithm,seed,run,scenario,src,dst,sport,dport,pg,size_bytes,start_time_ns,fct_ns,base_rtt_ns,path_hops,bottleneck_rate_bps,bdp_bytes,window_bytes,standalone_fct_ns");
	RequireOutput(pfc_file, pfc_output_file,
			"algorithm,seed,run,time_ns,node_id,node_type,if_index,event_type");
	metadata_file.open(summary_meta_file.c_str(), std::ios::out | std::ios::trunc);
	if (!metadata_file.is_open())
		NS_FATAL_ERROR("longhaul: cannot open metadata file: " << summary_meta_file);

	Config::SetDefault("ns3::QbbNetDevice::PauseTime", UintegerValue(pause_time));
	Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(enable_qcn));
	
	// 设置全局网卡处理延迟
	Config::SetDefault("ns3::QbbNetDevice::NicDelay", TimeValue(NanoSeconds(nic_delay_ns)));

	// set int_multi
	IntHop::multi = int_multi;
	// IntHeader::mode
	if (cc_mode == 7) // timely, use ts
		IntHeader::mode = IntHeader::TS;
	else if (cc_mode == 3) // hpcc uses INT
		IntHeader::mode = IntHeader::NORMAL;
	else
		IntHeader::mode = IntHeader::NONE;

	std::ifstream topof(topology_file.c_str());
	std::ifstream flowf(flow_file.c_str());
	if (!topof.is_open())
		NS_FATAL_ERROR("longhaul: cannot open topology file: " << topology_file);
	if (!flowf.is_open())
		NS_FATAL_ERROR("longhaul: cannot open flow file: " << flow_file);
	uint32_t node_num, switch_num, tor_count, link_num;
	if (!(topof >> node_num >> switch_num >> tor_count >> link_num))
		NS_FATAL_ERROR("longhaul: malformed topology header");

	std::vector<uint32_t> node_type(node_num, 0);
	for (uint32_t i = 0; i < switch_num; ++i) {
		uint32_t sid;
		if (!(topof >> sid) || sid >= node_num)
			NS_FATAL_ERROR("longhaul: malformed switch list in topology");
		node_type[sid] = i < tor_count ? 1 : 2;
	}

	for (uint32_t i = 0; i < node_num; ++i) {
		if (node_type[i] == 0) {
			n.Add(CreateObject<Node>());
			continue;
		}
		Ptr<SwitchNode> sw = CreateObject<SwitchNode>();
		sw->SetNodeType(node_type[i]);
		sw->SetAttribute("EcnEnabled", BooleanValue(enable_qcn));
		n.Add(sw);
	}

	InternetStackHelper internet;
	Ipv4GlobalRoutingHelper globalRoutingHelper;
	internet.SetRoutingHelper(globalRoutingHelper);
	internet.Install(n);
	AssignNodeAddresses();

	//
	// Explicitly create the channels required by the topology.
	//

	Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
	Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
	rem->SetRandomVariable(uv);
	uv->SetStream(50);
	rem->SetAttribute("ErrorRate", DoubleValue(error_rate_per_link));
	rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

	QbbHelper qbb;
	Ipv4AddressHelper ipv4;
	for (uint32_t i = 0; i < link_num; i++)
	{
		uint32_t src, dst;
		std::string link_rate, link_delay;
		double error_rate;
		if (!(topof >> src >> dst >> link_rate >> link_delay >> error_rate) ||
			src >= node_num || dst >= node_num || src == dst)
			NS_FATAL_ERROR("longhaul: malformed link " << i << " in topology");
		Ptr<Node> snode = n.Get(src), dnode = n.Get(dst);

		qbb.SetDeviceAttribute("DataRate", StringValue(link_rate));
		qbb.SetChannelAttribute("Delay", StringValue(link_delay));
		if (error_rate > 0)
		{
			Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
			Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
			rem->SetRandomVariable(uv);
			uv->SetStream(50);
			rem->SetAttribute("ErrorRate", DoubleValue(error_rate));
			rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
			qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
		}
		else
		{
			qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
		}

		// Install the node-wide address before assigning the per-link subnet address,
		// so it remains the primary address used by RDMA and routing.
		NetDeviceContainer d = qbb.Install(snode, dnode);
		if (snode->GetNodeType() == 0) {
			Ptr<Ipv4> ipv4 = snode->GetObject<Ipv4>();
			ipv4->AddInterface(d.Get(0));
			ipv4->AddAddress(1, Ipv4InterfaceAddress(serverAddress[src], Ipv4Mask(0xff000000)));
		}
		if (dnode->GetNodeType() == 0) {
			Ptr<Ipv4> ipv4 = dnode->GetObject<Ipv4>();
			ipv4->AddInterface(d.Get(1));
			ipv4->AddAddress(1, Ipv4InterfaceAddress(serverAddress[dst], Ipv4Mask(0xff000000)));
		}

		// Per-link subnets only provide interface connectivity; node-wide addresses
		// above are the addresses used by RDMA.
		std::stringstream ipstring;
		ipstring << "10." << i / 254 + 1 << "." << i % 254 + 1 << ".0";
		// sprintf(ipstring, "10.%d.%d.0", i / 254 + 1, i % 254 + 1);
		ipv4.SetBase(ipstring.str().c_str(), "255.255.255.0");
		ipv4.Assign(d);

		// used to create a graph of the topology
		Ptr<Ipv4> sIpv4 = snode->GetObject<Ipv4>();
		Ptr<Ipv4> dIpv4 = dnode->GetObject<Ipv4>();

		nbr2if[snode][dnode].idx = sIpv4->GetInterfaceForDevice(d.Get(0));
		nbr2if[snode][dnode].delay = DynamicCast<QbbChannel>(DynamicCast<QbbNetDevice>(d.Get(0))->GetChannel())->GetDelay().GetTimeStep();
		nbr2if[snode][dnode].bw = DynamicCast<QbbNetDevice>(d.Get(0))->GetDataRate().GetBitRate();
		nbr2if[dnode][snode].idx = dIpv4->GetInterfaceForDevice(d.Get(1));
		nbr2if[dnode][snode].delay = DynamicCast<QbbChannel>(DynamicCast<QbbNetDevice>(d.Get(1))->GetChannel())->GetDelay().GetTimeStep();
		nbr2if[dnode][snode].bw = DynamicCast<QbbNetDevice>(d.Get(1))->GetDataRate().GetBitRate();

		// setup PFC trace
		Ptr<QbbNetDevice> srcDev = DynamicCast<QbbNetDevice>(d.Get(0));
		Ptr<QbbNetDevice> dstDev = DynamicCast<QbbNetDevice>(d.Get(1));
		srcDev->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback (&get_pfc, &pfc_file, srcDev));
		dstDev->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback (&get_pfc, &pfc_file, dstDev));
		if (src == dci_left && dst == dci_right) {
			dci_left_device = srcDev;
			dci_right_device = dstDev;
			srcDev->TraceConnectWithoutContext("QbbDequeue", MakeBoundCallback (&CountDciEcn, srcDev));
			dstDev->TraceConnectWithoutContext("QbbDequeue", MakeBoundCallback (&CountDciEcn, dstDev));
		} else if (src == dci_right && dst == dci_left) {
			dci_right_device = srcDev;
			dci_left_device = dstDev;
			srcDev->TraceConnectWithoutContext("QbbDequeue", MakeBoundCallback (&CountDciEcn, srcDev));
			dstDev->TraceConnectWithoutContext("QbbDequeue", MakeBoundCallback (&CountDciEcn, dstDev));
		}
	}

	nic_rate = GetNicRate();
	// config switch
	// The switch mmu runs Dynamic Thresholds (DT) by default.
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType()) { // is switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));
			double alpha = 1.0 / 8;
			sw->m_mmu->SetAlphaIngress(alpha);
			sw->m_mmu->SetAlphaEgress(UINT16_MAX);
			uint64_t totalHeadroom = 0;
			for (uint32_t j = 1; j < sw->GetNDevices(); j++) {

				for (uint32_t qu = 0; qu < 8; qu++) {
					Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(sw->GetDevice(j));
					// set ecn
					uint64_t rate = dev->GetDataRate().GetBitRate();
					NS_ASSERT_MSG(rate2kmin.find(rate) != rate2kmin.end(), "must set kmin for each link speed");
					NS_ASSERT_MSG(rate2kmax.find(rate) != rate2kmax.end(), "must set kmax for each link speed");
					NS_ASSERT_MSG(rate2pmax.find(rate) != rate2pmax.end(), "must set pmax for each link speed");
					sw->m_mmu->ConfigEcn(j, rate2kmin[rate], rate2kmax[rate], rate2pmax[rate]);
					// set pfc
					uint64_t delay = DynamicCast<QbbChannel>(dev->GetChannel())->GetDelay().GetTimeStep();
					uint32_t headroom = rate * delay / 8 / 1000000000 * 3;

					sw->m_mmu->SetHeadroom(headroom, j, qu);
					totalHeadroom += headroom;
				}

			}
			sw->m_mmu->SetBufferPool(buffer_size * 1024 * 1024 + totalHeadroom);
			sw->m_mmu->SetIngressPool(buffer_size * 1024 * 1024);
			sw->m_mmu->SetEgressLosslessPool(buffer_size * 1024 * 1024);
			sw->m_mmu->node_id = sw->GetId();
		}
	}

#if ENABLE_QP
	//
	// install RDMA driver
	//
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() == 0) { // is server
			// create RdmaHw
			Ptr<RdmaHw> rdmaHw = CreateObject<RdmaHw>();
			rdmaHw->SetAttribute("ClampTargetRate", BooleanValue(clamp_target_rate));
			rdmaHw->SetAttribute("AlphaResumInterval", DoubleValue(alpha_resume_interval));
			rdmaHw->SetAttribute("RPTimer", DoubleValue(rp_timer));
			rdmaHw->SetAttribute("FastRecoveryTimes", UintegerValue(fast_recovery_times));
			rdmaHw->SetAttribute("EwmaGain", DoubleValue(ewma_gain));
			rdmaHw->SetAttribute("RateAI", DataRateValue(DataRate(rate_ai)));
			rdmaHw->SetAttribute("RateHAI", DataRateValue(DataRate(rate_hai)));
			rdmaHw->SetAttribute("L2BackToZero", BooleanValue(l2_back_to_zero));
			rdmaHw->SetAttribute("L2ChunkSize", UintegerValue(l2_chunk_size));
			rdmaHw->SetAttribute("L2AckInterval", UintegerValue(l2_ack_interval));
			rdmaHw->SetAttribute("CcMode", UintegerValue(cc_mode));
			rdmaHw->SetAttribute("RateDecreaseInterval", DoubleValue(rate_decrease_interval));
			rdmaHw->SetAttribute("MinRate", DataRateValue(DataRate(min_rate)));
			rdmaHw->SetAttribute("Mtu", UintegerValue(packet_payload_size));
			rdmaHw->SetAttribute("MiThresh", UintegerValue(mi_thresh));
			rdmaHw->SetAttribute("VarWin", BooleanValue(var_win));
			rdmaHw->SetAttribute("FastReact", BooleanValue(fast_react));
			rdmaHw->SetAttribute("MultiRate", BooleanValue(multi_rate));
			rdmaHw->SetAttribute("SampleFeedback", BooleanValue(sample_feedback));
			rdmaHw->SetAttribute("TargetUtil", DoubleValue(u_target));
			rdmaHw->SetAttribute("RateBound", BooleanValue(rate_bound));
			rdmaHw->SetAttribute("DctcpRateAI", DataRateValue(DataRate(dctcp_rate_ai)));
			rdmaHw->SetAttribute("TimelyAlpha", DoubleValue(timely_alpha));
			rdmaHw->SetAttribute("TimelyBeta", DoubleValue(timely_beta));
			rdmaHw->SetAttribute("TimelyTLow", UintegerValue(timely_tlow_ns));
			rdmaHw->SetAttribute("TimelyTHigh", UintegerValue(timely_thigh_ns));
			rdmaHw->SetAttribute("TimelyMinRtt", UintegerValue(timely_min_rtt_ns));
			// longhaul-convergence owns the structured sender sampler below.
			rdmaHw->SetAttribute("TxRateSampleInterval", DoubleValue(0.0));
			rdmaHw->SetAttribute("PowerTCPEnabled", BooleanValue(false));
			rdmaHw->SetAttribute("PowerTCPdelay", BooleanValue(false));
			// create and install RdmaDriver
			Ptr<RdmaDriver> rdma = CreateObject<RdmaDriver>();
			Ptr<Node> node = n.Get(i);
			rdma->SetNode(node);
			rdma->SetRdmaHw(rdmaHw);

			node->AggregateObject (rdma);
			rdma->Init();
			rdma->TraceConnectWithoutContext("QpComplete", MakeCallback (qp_finish));
		}
	}

#endif

	// set ACK priority on hosts
	if (ack_high_prio)
		RdmaEgressQueue::ack_q_idx = 0;
	else
		RdmaEgressQueue::ack_q_idx = 3;

	// Node addresses are assigned before links; now install the static RDMA routes.
	CalculateRoutes(n);
	SetRoutingEntries();
	//
	// Compute per-path base RTT and BDP.
	//
	maxRtt = 0;
	uint64_t minRtt = 1e9;
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() != 0)
			continue;
		for (uint32_t j = 0; j < node_num; j++) {
			if (n.Get(j)->GetNodeType() != 0)
				continue;
			if (i == j)
				continue;
			uint64_t delay = pairDelay[n.Get(i)][n.Get(j)];
			uint64_t hops = pairHopCount[n.Get(i)][n.Get(j)];
			uint64_t rtt = delay * 2 + 2 * hops * nic_delay_ns;
			uint64_t bw = pairBw[i][j];
			uint64_t bdp = static_cast<uint64_t>(static_cast<__uint128_t>(rtt) * bw / 1000000000ULL / 8ULL);
			pairBdp[n.Get(i)][n.Get(j)] = bdp;
			pairBaseRtt[i][j] = rtt;
			if (rtt > maxRtt)
				maxRtt = rtt;
			if (rtt < minRtt)
				minRtt = rtt;
		}
	}
	printf("maxBaseRtt=%lu minBaseRtt=%lu\n", maxRtt, uint64_t(minRtt));

	//
	// setup switch CC
	//
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType()) { // switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));

			sw->SetAttribute("CcMode", UintegerValue(cc_mode));
			
			sw->SetAttribute("MaxRtt", UintegerValue(maxRtt));
			sw->SetAttribute("PowerEnabled", BooleanValue(false));
			if (cc_mode == 13) {
				sw->SetAttribute("SwitchFeedbackEnabled", BooleanValue(true));
				sw->StartPeriodicFeedbackMechanism(MicroSeconds(40));
			}

		}
	}

	Ipv4GlobalRoutingHelper::PopulateRoutingTables();

	LoadFlows(flowf);
	if (dci_left_device == NULL || dci_right_device == NULL)
		NS_FATAL_ERROR("longhaul: could not identify the DCI link " << dci_left << " <-> " << dci_right);
	dci_left_tx_bytes = LinkTxCounter(dci_left_device);
	dci_right_tx_bytes = LinkTxCounter(dci_right_device);
	dci_last_time_ns = Simulator::Now().GetNanoSeconds();
	dci_sample_initialized = true;
	for (uint32_t i = 0; i < flows.size(); ++i)
		Simulator::Schedule(Seconds(flows[i].start_time), &StartFlow, i);
	ResearchSetup();
	WriteMetadata(node_num, switch_num, link_num);

	topof.close();
	flowf.close();
	
	uint64_t flow_sample_interval_us = std::min(rate_sample_interval_us,
						std::min(goodput_sample_interval_us, rtt_sample_interval_us));
	Simulator::Schedule(MicroSeconds(flow_sample_interval_us), &SampleFlowRates);
	Simulator::Schedule(MicroSeconds(rate_sample_interval_us), &SampleDciLink);

	std::cout << "Running Simulation.\n";
	NS_LOG_INFO("Run Simulation.");
	Simulator::Stop(Seconds(simulator_stop_time));
	Simulator::Run();
	Simulator::Destroy();
	sender_rate_file.close();
	receiver_goodput_file.close();
	link_stats_file.close();
	rtt_file.close();
	fct_file.close();
	pfc_file.close();
	metadata_file.close();
	NS_LOG_INFO("Done.");
	endt = clock();
	std::cout << (double)(endt - begint) / CLOCKS_PER_SEC << "\n";
}
