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
#include <unordered_map>
#include <algorithm>
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
uint32_t packet_payload_size = 1000, l2_chunk_size = 0, l2_ack_interval = 0;
double pause_time = 5, simulator_stop_time = 3.01;
std::string topology_file, flow_file;
std::string fct_output_file = "fct.txt";
std::string pfc_output_file = "pfc.txt";
std::string sender_rate_output_file = "sender-rate.csv";
std::string receiver_goodput_output_file = "receiver-goodput.csv";
std::string link_stats_output_file = "dci-link.csv";
std::string summary_meta_file = "metadata.json";
uint64_t rate_sample_interval_us = 100;
uint64_t goodput_sample_interval_us = 100;
uint32_t rng_seed = 1;
uint64_t rng_run = 1;
uint32_t dci_left = 40, dci_right = 81;
std::string scenario_name = "unknown";

double alpha_resume_interval = 55, rp_timer, ewma_gain = 1 / 16;
double timely_alpha = 0.875, timely_beta = 0.8;
uint64_t timely_tlow_ns = 5000000, timely_thigh_ns = 10000000, timely_min_rtt_ns = 10000000;
double rate_decrease_interval = 4;
uint32_t fast_recovery_times = 5;
std::string rate_ai, rate_hai, min_rate = "100Mb/s";
std::string dctcp_rate_ai = "1000Mb/s";

bool clamp_target_rate = false, l2_back_to_zero = false;
double error_rate_per_link = 0.0;
uint32_t has_win = 1;
uint32_t global_t = 1;
uint32_t mi_thresh = 5;
bool var_win = false, fast_react = true;
bool multi_rate = true;
bool sample_feedback = false;
double u_target = 0.95;
uint32_t int_multi = 1;
bool rate_bound = true;

uint32_t ack_high_prio = 0;
uint32_t buffer_size = 16;

// 网卡处理延迟（纳秒）
uint64_t nic_delay_ns = 15000;  // 默认15μs = 15000ns

unordered_map<uint64_t, uint32_t> rate2kmax, rate2kmin;
unordered_map<uint64_t, double> rate2pmax;

/************************************************
 * Runtime varibles
 ***********************************************/
NodeContainer n;

uint64_t nic_rate;

uint64_t maxRtt, maxBdp;

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
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairTxDelay;
map<uint32_t, map<uint32_t, uint64_t> > pairBw;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairBdp;
map<uint32_t, map<uint32_t, uint64_t> > pairRtt;

std::vector<Ipv4Address> serverAddress;

std::ofstream sender_rate_file;
std::ofstream receiver_goodput_file;
std::ofstream link_stats_file;
std::ofstream fct_file;
std::ofstream pfc_file;
std::ofstream metadata_file;

Ptr<QbbNetDevice> dci_left_device;
Ptr<QbbNetDevice> dci_right_device;
uint64_t dci_last_time_ns = 0;
uint64_t dci_left_tx_bytes = 0, dci_left_rx_bytes = 0;
uint64_t dci_right_tx_bytes = 0, dci_right_rx_bytes = 0;
uint64_t dci_left_ecn_events = 0, dci_right_ecn_events = 0;
uint64_t pfc_pause_events = 0, pfc_resume_events = 0;

// maintain port number for each host pair
std::unordered_map<uint32_t, unordered_map<uint32_t, uint16_t> > nextPort;

struct FlowInput {
	uint64_t src, dst, pg, maxPacketCount, port, dport;
	double start_time;
	uint64_t last_recv_bytes;
	uint64_t last_tx_seq;
	uint64_t last_tx_time_ns;
	uint64_t last_rx_time_ns;
	bool tx_initialized;
	bool rx_initialized;
	bool finished;
};
std::vector<FlowInput> flows;

uint32_t ip_to_node_id(Ipv4Address ip);

std::string selected_cc;
std::string AlgorithmName(uint32_t mode) {
	if (selected_cc == "proposed") return "proposed";
	if (mode == 1) return "dcqcn";
	if (mode == 3) return "hpcc";
	if (mode == 7) return "timely";
	return "cc-mode-" + std::to_string(mode);
}

void RequireOutput(std::ofstream &file, const std::string &path, const std::string &header) {
	file.open(path.c_str(), std::ios::out | std::ios::trunc);
	if (!file.is_open()) {
		NS_FATAL_ERROR("longhaul: cannot open output file: " << path);
	}
	file << header << '\n';
}

void WriteRate(std::ofstream &file, uint32_t src, uint32_t dst, uint16_t sport,
		       uint16_t dport, uint16_t pg, uint64_t time_ns, double value_bps) {
	if (file.is_open()) {
		file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ','
		      << time_ns << ',' << src << ',' << dst << ',' << sport << ',' << dport
		      << ',' << pg << ',' << std::fixed << std::setprecision(3) << value_bps << '\n';
	}
}

void LoadFlows(std::istream &input) {
	uint32_t flow_count;
	if (!(input >> flow_count))
		NS_FATAL_ERROR("longhaul: flow file is missing its flow count");

	flows.reserve(flow_count);
	for (uint32_t i = 0; i < flow_count; ++i) {
		FlowInput flow{};
		if (!(input >> flow.src >> flow.dst >> flow.pg >> flow.dport >>
		      flow.maxPacketCount >> flow.start_time)) {
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
		? (global_t == 1 ? maxBdp : pairBdp[n.Get(flow.src)][n.Get(flow.dst)])
		: 0;
	uint64_t rtt = global_t == 1 ? maxRtt : pairRtt[flow.src][flow.dst];
	RdmaClientHelper clientHelper(flow.pg, serverAddress[flow.src], serverAddress[flow.dst],
		flow.port, flow.dport, flow.maxPacketCount, window, rtt,
		Simulator::GetMaximumSimulationTime());
	ApplicationContainer applications = clientHelper.Install(n.Get(flow.src));
	// The callback itself is scheduled at flow.start_time, so the app starts now.
	applications.Start(Seconds(0));
}

// 全局映射表：IP地址 -> 节点ID
map<uint32_t, uint32_t> ipToNodeIdMap;  // key: IP (uint32_t), value: nodeId

Ipv4Address NodeAddress(uint32_t node_id) {
	NS_ABORT_MSG_IF(node_id >= 254, "longhaul supports at most 253 node IDs");
	std::string address = "11.0.0." + std::to_string(node_id + 1);
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

void RecordCompletionSamples(Ptr<RdmaQueuePair> q, FlowInput *fi) {
	if (fi == NULL) return;
	uint64_t now_ns = Simulator::Now().GetNanoSeconds();
	if (fi->tx_initialized && now_ns > fi->last_tx_time_ns && q->snd_nxt >= fi->last_tx_seq) {
		double rate = double(q->snd_nxt - fi->last_tx_seq) * 8.0 * 1e9 /
				      double(now_ns - fi->last_tx_time_ns);
		WriteRate(sender_rate_file, fi->src, fi->dst, q->sport, q->dport, q->m_pg, now_ns, rate);
	}
	Ptr<RdmaRxQueuePair> rxQp = FindRxQp(*fi);
	if (rxQp != NULL && fi->rx_initialized && now_ns > fi->last_rx_time_ns &&
	    rxQp->m_recv_bytes >= fi->last_recv_bytes) {
		double rate = double(rxQp->m_recv_bytes - fi->last_recv_bytes) * 8.0 * 1e9 /
				      double(now_ns - fi->last_rx_time_ns);
		WriteRate(receiver_goodput_file, fi->src, fi->dst, q->sport, q->dport, q->m_pg, now_ns, rate);
	}
	fi->finished = true;
}

void qp_finish(Ptr<RdmaQueuePair> q) {
	uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
	uint64_t base_rtt = pairRtt[sid][did], b = pairBw[sid][did];
	uint64_t total_bytes = q->m_size + ((q->m_size - 1) / packet_payload_size + 1) *
		(CustomHeader::GetStaticWholeHeaderSize() - IntHeader::GetStaticSize());
	uint64_t standalone_fct = base_rtt + total_bytes * 8000000000lu / b;
	FlowInput *fi = FindFlow(q);
	RecordCompletionSamples(q, fi);
	fct_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ','
			  << scenario_name << ',' << sid << ',' << did << ',' << q->sport << ',' << q->dport
			  << ',' << q->m_pg << ',' << q->m_size << ',' << q->startTime.GetNanoSeconds()
			  << ',' << (Simulator::Now() - q->startTime).GetNanoSeconds() << ',' << standalone_fct << '\n';
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
	map<Ptr<Node>, uint64_t> txDelay;
	map<Ptr<Node>, uint64_t> bw;
	// init BFS.
	q.push_back(host);
	dis[host] = 0;
	delay[host] = 0;
	txDelay[host] = 0;
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
				txDelay[next] = txDelay[now] + packet_payload_size * 1000000000lu * 8 / it->second.bw;
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
	for (auto it : txDelay)
		pairTxDelay[it.first][host] = it.second;
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
				fi.last_tx_seq = txQp->snd_nxt;
				fi.last_tx_time_ns = now_ns;
			} else if (now_ns > fi.last_tx_time_ns &&
				       now_ns - fi.last_tx_time_ns >= rate_sample_interval_us * 1000 &&
				       txQp->snd_nxt >= fi.last_tx_seq) {
				double rate = double(txQp->snd_nxt - fi.last_tx_seq) * 8.0 * 1e9 /
						  double(now_ns - fi.last_tx_time_ns);
				WriteRate(sender_rate_file, fi.src, fi.dst, txQp->sport, txQp->dport, txQp->m_pg, now_ns, rate);
				fi.last_tx_seq = txQp->snd_nxt;
				fi.last_tx_time_ns = now_ns;
			}
		}

		Ptr<RdmaRxQueuePair> rxQp = FindRxQp(fi);
		if (rxQp != NULL) {
			if (!fi.rx_initialized) {
				fi.rx_initialized = true;
				fi.last_recv_bytes = rxQp->m_recv_bytes;
				fi.last_rx_time_ns = now_ns;
			} else if (now_ns > fi.last_rx_time_ns &&
				       now_ns - fi.last_rx_time_ns >= goodput_sample_interval_us * 1000 &&
				       rxQp->m_recv_bytes >= fi.last_recv_bytes) {
				double rate = double(rxQp->m_recv_bytes - fi.last_recv_bytes) * 8.0 * 1e9 /
						  double(now_ns - fi.last_rx_time_ns);
				WriteRate(receiver_goodput_file, fi.src, fi.dst, (uint16_t)fi.port, fi.dport,
						  (uint16_t)fi.pg, now_ns, rate);
				fi.last_recv_bytes = rxQp->m_recv_bytes;
				fi.last_rx_time_ns = now_ns;
			}
		}
	}
	uint64_t interval_us = std::min(rate_sample_interval_us, goodput_sample_interval_us);
	if (Simulator::Now() + MicroSeconds(interval_us) <= Seconds(simulator_stop_time))
		Simulator::Schedule(MicroSeconds(interval_us), &SampleFlowRates);
}

uint64_t LinkCounter(Ptr<QbbNetDevice> dev, bool tx) {
	if (dev == NULL) return 0;
	return tx ? dev->totalBytesSent : dev->totalBytesRcvd;
}

uint64_t LinkQueueBytes(Ptr<QbbNetDevice> dev) {
	if (dev == NULL || dev->GetQueue() == NULL) return 0;
	return dev->GetQueue()->GetNBytesTotal();
}

void SampleDciLink() {
	uint64_t now_ns = Simulator::Now().GetNanoSeconds();
	if (dci_left_device != NULL && dci_right_device != NULL && dci_last_time_ns != 0 && now_ns > dci_last_time_ns) {
		double dt = double(now_ns - dci_last_time_ns) * 1e-9;
		uint64_t left_tx = LinkCounter(dci_left_device, true), left_rx = LinkCounter(dci_left_device, false);
		uint64_t right_tx = LinkCounter(dci_right_device, true), right_rx = LinkCounter(dci_right_device, false);
		link_stats_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ',' << now_ns << ','
						<< dci_left << ',' << dci_right << ',' << "left-to-right" << ','
						<< ((left_tx - dci_left_tx_bytes) * 8.0 / dt) << ','
						<< ((left_rx - dci_left_rx_bytes) * 8.0 / dt) << ',' << LinkQueueBytes(dci_left_device) << ','
						<< dci_left_ecn_events << ',' << pfc_pause_events << ',' << pfc_resume_events << '\n';
		link_stats_file << AlgorithmName(cc_mode) << ',' << rng_seed << ',' << rng_run << ',' << now_ns << ','
						<< dci_right << ',' << dci_left << ',' << "right-to-left" << ','
						<< ((right_tx - dci_right_tx_bytes) * 8.0 / dt) << ','
						<< ((right_rx - dci_right_rx_bytes) * 8.0 / dt) << ',' << LinkQueueBytes(dci_right_device) << ','
						<< dci_right_ecn_events << ',' << pfc_pause_events << ',' << pfc_resume_events << '\n';
		link_stats_file.flush();
		dci_left_tx_bytes = left_tx; dci_left_rx_bytes = left_rx;
		dci_right_tx_bytes = right_tx; dci_right_rx_bytes = right_rx;
	}
	dci_last_time_ns = now_ns;
	if (Simulator::Now() + MicroSeconds(rate_sample_interval_us) <= Seconds(simulator_stop_time))
		Simulator::Schedule(MicroSeconds(rate_sample_interval_us), &SampleDciLink);
}

#include "longhaul-research.h"

void WriteMetadata(uint32_t node_num, uint32_t switch_num, uint32_t link_num, uint64_t representative_rtt_ns) {
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
		<< "  \"representative_rtt_ns\": " << representative_rtt_ns << ",\n"
		<< "  \"rate_sample_interval_us\": " << rate_sample_interval_us << ",\n"
		<< "  \"goodput_sample_interval_us\": " << goodput_sample_interval_us << ",\n"
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
		<< "  \"flow_file\": \"" << flow_file << "\"\n"
		<< "}\n";
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
		else if (key == "CC_MODE") ReadConfigValue(config, key, cc_mode);
		else if (key == "RNG_SEED") ReadConfigValue(config, key, rng_seed);
		else if (key == "RNG_RUN") ReadConfigValue(config, key, rng_run);
		else if (key == "SCENARIO") ReadConfigValue(config, key, scenario_name);
		else if (key == "RATE_SAMPLE_INTERVAL_US") ReadConfigValue(config, key, rate_sample_interval_us);
		else if (key == "GOODPUT_SAMPLE_INTERVAL_US") ReadConfigValue(config, key, goodput_sample_interval_us);
		else if (key == "RATE_OUTPUT_FILE") ReadConfigValue(config, key, sender_rate_output_file);
		else if (key == "GOODPUT_OUTPUT_FILE") ReadConfigValue(config, key, receiver_goodput_output_file);
		else if (key == "LINK_STATS_OUTPUT_FILE") ReadConfigValue(config, key, link_stats_output_file);
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
		else if (key == "GLOBAL_T") ReadConfigValue(config, key, global_t);
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
		else NS_FATAL_ERROR("longhaul: unknown config key: " << key);
	}
}

int main(int argc, char *argv[])
{
	clock_t begint, endt;
	begint = clock();
	std::ifstream conf;
	uint32_t algorithm = 0;
	uint32_t windowCheck = std::numeric_limits<uint32_t>::max();
	std::string confFile = "examples/LonghaulCC/config-longhaul-common.txt";
	CommandLine cmd;
	cmd.AddValue("cc", "dcqcn, hpcc, timely, or proposed (report prototype)", selected_cc);
	cmd.AddValue("proposedPeriod", "receiver control period in seconds", research_period);
	cmd.AddValue("proposedNearPeriod", "near-source period in seconds", research_near_period);
	cmd.AddValue("proposedQref", "queue reference in bytes", research_qref);
	cmd.AddValue("proposedWeight", "forecast weight in [0,1]", research_forecast_weight);
	cmd.AddValue("researchReceiver", "directly attached receiver host", research_receiver);
	cmd.AddValue("researchControl", "0 baseline, 1 reactive, 2 predictive", research_control);
	cmd.AddValue("researchOutput", "receiver bottleneck CSV path", research_output);
	cmd.AddValue("conf", "config file path", confFile);
	cmd.AddValue ("algorithm", "specify CC mode. This is added for my convinience. I prefer cmd rather than parsing files.", algorithm);
	cmd.AddValue("windowCheck", "windowCheck", windowCheck);
	cmd.AddValue("seed", "ns-3 RNG seed", rng_seed);
	cmd.AddValue("run", "ns-3 RNG run", rng_run);
	cmd.AddValue("scenario", "scenario name recorded in metadata", scenario_name);

	cmd.Parse (argc, argv);
	if (selected_cc == "proposed") {
		research_period = 0.0002;
		research_near_period = 0.00005;
		research_qref = 250000;
		research_guarded = true;
		research_control = 2;
	}
	conf.open(confFile.c_str());
	if (!conf.is_open())
		NS_FATAL_ERROR("longhaul: cannot open config file: " << confFile);
	ParseConfig(conf);
	conf.close();
	// Re-parse so explicit command-line values also override values read from
	// the common config template (including seed/run and output metadata).
	cmd.Parse(argc, argv);

	// Command line values override the config only when explicitly supplied.
	if (algorithm != 0) cc_mode = algorithm;
	if (!selected_cc.empty()) {
		uint32_t requested = selected_cc == "hpcc" ? 3 : selected_cc == "timely" ? 7 : 1;
		if (selected_cc != "dcqcn" && selected_cc != "hpcc" && selected_cc != "timely" && selected_cc != "proposed")
			NS_FATAL_ERROR("unknown --cc: " << selected_cc);
		if (algorithm && algorithm != requested) NS_FATAL_ERROR("conflicting --cc and --algorithm");
		cc_mode = requested;
		if (selected_cc == "proposed") {
			if (research_control != 1 && research_control != 2) NS_FATAL_ERROR("proposed requires researchControl 1 or 2");
			if (research_output.empty()) research_output = summary_meta_file + ".control.csv";
		} else if (research_control) NS_FATAL_ERROR("use --cc=proposed for research control");
	}
	if (!std::isfinite(research_period) || research_period <= 0 || !std::isfinite(research_near_period) || research_near_period <= 0 || !std::isfinite(research_qref) || research_qref <= 0 || !std::isfinite(research_forecast_weight) || research_forecast_weight < 0 || research_forecast_weight > 1)
		NS_FATAL_ERROR("invalid proposed period, queue reference, or forecast weight");
	if (windowCheck != std::numeric_limits<uint32_t>::max()) {
		has_win = windowCheck;
		var_win = windowCheck;
	}
	if (research_control > 2 || (research_control && research_output.empty()))
		NS_FATAL_ERROR("invalid research control or missing output");
	if (cc_mode != 1 && cc_mode != 3 && cc_mode != 7)
		NS_FATAL_ERROR("longhaul supports only DCQCN(1), HPCC(3), and TIMELY(7)");
	if (rate_sample_interval_us == 0 || goodput_sample_interval_us == 0)
		NS_FATAL_ERROR("longhaul sampling intervals must be positive");
	// Longhaul writes measurements to CSV; keep historical model diagnostics out
	// of the run log unless another program explicitly leaves this disabled.
	g_longhaul_quiet = true;
	RngSeedManager::SetSeed(rng_seed);
	RngSeedManager::SetRun(rng_run);
	RequireOutput(sender_rate_file, sender_rate_output_file,
			"algorithm,seed,run,time_ns,src,dst,sport,dport,pg,value_bps");
	RequireOutput(receiver_goodput_file, receiver_goodput_output_file,
			"algorithm,seed,run,time_ns,src,dst,sport,dport,pg,value_bps");
	RequireOutput(link_stats_file, link_stats_output_file,
			"algorithm,seed,run,time_ns,src,dst,direction,tx_bps,rx_bps,queue_bytes,ecn_events,pfc_pause_events,pfc_resume_events");
	RequireOutput(fct_file, fct_output_file,
			"algorithm,seed,run,scenario,src,dst,sport,dport,pg,size_bytes,start_time_ns,fct_ns,standalone_fct_ns");
	RequireOutput(pfc_file, pfc_output_file,
			"algorithm,seed,run,time_ns,node_id,node_type,if_index,event_type");
	metadata_file.open(summary_meta_file.c_str(), std::ios::out | std::ios::trunc);
	if (!metadata_file.is_open())
		NS_FATAL_ERROR("longhaul: cannot open metadata file: " << summary_meta_file);


	Config::SetDefault("ns3::QbbNetDevice::PauseTime", UintegerValue(pause_time));
	Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(enable_qcn));
	
	// 设置全局网卡处理延迟
	Config::SetDefault("ns3::QbbNetDevice::NicDelay", TimeValue(NanoSeconds(nic_delay_ns)));
	NS_LOG_INFO("NIC delay set to " << nic_delay_ns << "ns (" << (nic_delay_ns / 1000.0) << "μs)");

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
	if (node_num != n.GetN() && n.GetN() != 0)
		NS_FATAL_ERROR("longhaul: internal node count mismatch");

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
		// 【修复坑1】必须在ipv4.Assign(d)之后调用GetInterfaceForDevice
		// 因为Assign才会把NetDevice注册到Ipv4协议栈，之前GetInterfaceForDevice返回-1
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
	// get BDP and delay
	//
	maxRtt = maxBdp = 0;
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
			uint64_t txDelay = pairTxDelay[n.Get(i)][n.Get(j)];
			uint64_t rtt = delay * 2 + txDelay;
			uint64_t bw = pairBw[i][j];
			uint64_t bdp = rtt * bw / 1000000000 / 8;
			pairBdp[n.Get(i)][n.Get(j)] = bdp;
			pairRtt[i][j] = rtt;
			if (bdp > maxBdp)
				maxBdp = bdp;
			if (rtt > maxRtt)
				maxRtt = rtt;
			if (rtt < minRtt)
				minRtt = rtt;
		}
	}
	printf("maxRtt=%lu maxBdp=%lu minRtt=%lu\n", maxRtt, maxBdp, uint64_t(minRtt));

	//
	// setup switch CC
	//
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType()) { // switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));

			sw->SetAttribute("CcMode", UintegerValue(cc_mode));
			
			sw->SetAttribute("MaxRtt", UintegerValue(maxRtt));
			sw->SetAttribute("PowerEnabled", BooleanValue(false));

		}
	}

	Ipv4GlobalRoutingHelper::PopulateRoutingTables();

	LoadFlows(flowf);
	for (uint32_t i = 0; i < flows.size(); ++i)
		Simulator::Schedule(Seconds(flows[i].start_time), &StartFlow, i);
	if (dci_left_device == NULL || dci_right_device == NULL)
		NS_FATAL_ERROR("longhaul: could not identify the DCI link " << dci_left << " <-> " << dci_right);
	uint64_t representative_rtt_ns = 0;
	if (!flows.empty())
		representative_rtt_ns = pairRtt[flows[0].src][flows[0].dst];
	ResearchSetup();
	WriteMetadata(node_num, switch_num, link_num, representative_rtt_ns);

	topof.close();
	flowf.close();
	
	Simulator::Schedule(MicroSeconds(goodput_sample_interval_us), &SampleFlowRates);
	Simulator::Schedule(MicroSeconds(rate_sample_interval_us), &SampleDciLink);

	std::cout << "Running Simulation.\n";
	NS_LOG_INFO("Run Simulation.");
	Simulator::Stop(Seconds(simulator_stop_time));
	Simulator::Run();
	Simulator::Destroy();
	sender_rate_file.close();
	receiver_goodput_file.close();
	link_stats_file.close();
	fct_file.close();
	pfc_file.close();
	metadata_file.close();
	NS_LOG_INFO("Done.");
	endt = clock();
	std::cout << (double)(endt - begint) / CLOCKS_PER_SEC << "\n";
}
