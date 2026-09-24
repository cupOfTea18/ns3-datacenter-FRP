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

#undef PGO_TRAINING
#define PATH_TO_PGO_CONFIG "path_to_pgo_config"

#include <iostream>
#include <fstream>
#include <unordered_map>
#include <time.h>
#include "ns3/core-module.h"
#include "ns3/qbb-helper.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/applications-module.h"
#include "ns3/internet-module.h"
#include "ns3/global-route-manager.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/packet.h"
#include "ns3/error-model.h"
#include <ns3/rdma.h>
#include <ns3/rdma-client.h>
#include <ns3/rdma-client-helper.h>
#include <ns3/rdma-driver.h>
#include <ns3/switch-node.h>
#include <ns3/sim-setting.h>
#include <ns3/switch-node.h>
#include "ns3/qbb-header.h"
#include <unordered_set>  // 添加这行

using namespace ns3;
using namespace std;

NS_LOG_COMPONENT_DEFINE("GENERIC_SIMULATION");

#define MAX_PRINT_CNT  100000000//不超过1G日志文件
uint32_t LogIntervalUs = 10; // 写日志频率,us
uint32_t PrintCnt = 0;

bool var_rate_flag = true;
int32_t var_rate_win = -111;
bool var_rate_desc = true;
uint32_t var_rate_inter = 1000; // 6us*1000=6ms

uint32_t cc_mode = 1;
bool enable_qcn = true;
bool enable_pfc   = true;
bool simulator_stop_one_flow = false;

// irn
bool enable_irn   = false;
std::string irn_rto_min = "30us"; // "30us"; 最小RTO，自适应DC内 RTO
std::string irn_rto_max = "11ms";// "11ms"; //"100us"; 最大RTO，自适应长距 RTO
uint32_t irn_bdp = 100*1000*1000*1000*(11/1000)/8; //bytes，网卡最大可连续发送字节数，按最大bdp计算(100Gbpsx11ms)

uint32_t packet_payload_size = 1000, l2_chunk_size = 0, l2_ack_interval = 0, l2timeout = 5;
double pause_time = 5, simulator_stop_time = 3.01;
std::string data_rate, link_delay, topology_file, flow_file, trace_file, trace_output_file;
std::string fct_output_file = "fct.txt";
std::string pfc_output_file = "pfc.txt";

double alpha_resume_interval = 55, rp_timer, ewma_gain = 1019.0;
double rate_decrease_interval = 4;
uint32_t fast_recovery_times = 5;
std::string rate_ai, rate_hai, min_rate = "100Mb/s";
std::string dctcp_rate_ai = "1000Mb/s";

uint32_t cnpGenerationInterval = 4; //us
bool clamp_target_rate = false, l2_back_to_zero = false;
bool byteCounterEnabled = true;
uint64_t byteCounterThreshold = 32767;
double error_rate_per_link = 0.0;
uint32_t has_win = 1;
uint32_t global_t = 1;
uint32_t mi_thresh = 5;
bool var_win = false, fast_react = true;
bool multi_rate = true;
bool sample_feedback = false;
double pint_log_base = 1.05;
double pint_prob = 1.0;
double u_target = 0.95;
uint32_t int_multi = 1;
bool rate_bound = true;

uint32_t ack_high_prio = 0;
uint64_t link_down_time = 0;
uint32_t link_down_A = 0, link_down_B = 0;

uint32_t enable_trace = 1;

uint32_t buffer_size = 16;

uint32_t qlen_dump_interval = 100000000, qlen_mon_interval = 100;
uint64_t qlen_mon_start = 2000000000, qlen_mon_end = 2100000000;
string qlen_mon_file;
unordered_map<uint64_t, uint32_t> rate2kmax, rate2kmin;
unordered_map<uint64_t, double> rate2pmax;

// bifrost para
bool bifrostFlag = false;
uint32_t bifrostTime = 10,bifrostK = 1;
unordered_map<uint32_t, uint32_t> bifrostLinks;
unordered_map<Ptr<QbbNetDevice>, bool> bifrostDevice;

// wrdma gateway para
uint32_t gatewayFlag = 0; // 0: not gateway, 1: wRdmaGateway, 2: atc gateway
uint32_t gatewayWin = 5, gatewayL2Timeout = 5;
uint32_t gatewaySrcPoolSize = 1,gatewayDstPoolSize = 1;
unordered_map<uint32_t, int> gatewayNodeType;
uint64_t gatewayPeerOutputRate = 0;
unordered_map<uint32_t, uint32_t> gatewayLongHaulDev;
bool gatewayXorEnable = false;
bool gatewaySrEnable = false;

// atc gateway para
uint32_t max_voq_count = 128;
uint64_t max_voq_rate =  100e9;    // 最大VOQ限速
uint64_t longHaulBandwidth = 200e9;    // 长链路带宽
Time longHaulDelay= MicroSeconds(5000); // 长链路时延

// lr2 gateway para
uint64_t lr2DepotReorderingSize = 1000000;  // Depot重排序缓冲区大小（字节）
uint64_t lr2DepotBackupSize = 50000;        // Depot备份池大小（字节）
uint32_t lr2DepotBackupTimeout = 100;       // Depot备份超时时间（微秒）

/************************************************
 * Runtime varibles
 ***********************************************/
std::ifstream topof, flowf, tracef;

NodeContainer n;
NetDeviceContainer switchToSwitchInterfaces;
std::map< uint32_t, std::map< uint32_t, std::vector<Ptr<QbbNetDevice>> > > switchToSwitch;

// vamsi
std::map<uint32_t, uint32_t> switchNumToId;
std::map<uint32_t, uint32_t> switchIdToNum;
std::map<uint32_t, NetDeviceContainer> switchUp;
std::map<uint32_t, NetDeviceContainer> switchDown;
//NetDeviceContainer switchUp[switch_num];
std::map<uint32_t, NetDeviceContainer> sourceNodes;

NodeContainer servers;
NodeContainer tors;
NodeContainer switchNodes;

uint64_t nic_rate;

uint64_t maxRtt, maxBdp;

struct Interface {
	uint32_t idx;
	bool up;
	uint64_t delay;
	uint64_t bw;

	Interface() : idx(0), up(false) {}
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

// maintain port number for each host pair
std::unordered_map<uint32_t, unordered_map<uint32_t, uint16_t> > portNumder;

struct FlowInput {
	uint64_t src, dst, pg, maxPacketCount, port, dport;
	double start_time;
	uint32_t idx;
	uint32_t flows_rand_inner , inner_src_begin,  inner_src_end, inner_dst_begin,inner_dst_end;
	uint32_t flows_rand_inter , inter_src_begin,  inter_src_end, inter_dst_begin,inter_dst_end;
	uint64_t flows_rand_packets;
	double flows_rand_start_time;
};
FlowInput flow_input = {0};
uint32_t flow_num;
std::vector<FlowInput> flows;
unordered_map<uint64_t, uint64_t> qpRecvSize;
std::unordered_set<uint64_t> flow_rand_set;

void ReadFlowInput() {
	if (flow_input.idx < flow_num) {
		flowf >> flow_input.src >> flow_input.dst >> flow_input.pg >> flow_input.dport >> flow_input.maxPacketCount >> flow_input.start_time;
		std::cout << "Flow " << flow_input.src << " " << flow_input.dst << " " << flow_input.pg << " " << flow_input.dport << " " << flow_input.maxPacketCount << " " << flow_input.start_time << " " << Simulator::Now().GetSeconds() << std::endl;
		NS_ASSERT(n.Get(flow_input.src)->GetNodeType() == 0 && n.Get(flow_input.dst)->GetNodeType() == 0);
	}
}
void ScheduleFlowInputs() {
	while (flow_input.idx < flow_num && Seconds(flow_input.start_time) <= Simulator::Now()) {
		uint32_t port = portNumder[flow_input.src][flow_input.dst]++; // get a new port number
		flow_input.port = port;
		flows.push_back(flow_input);
		RdmaClientHelper clientHelper(flow_input.pg, serverAddress[flow_input.src], serverAddress[flow_input.dst], port, flow_input.dport, flow_input.maxPacketCount, has_win ? (global_t == 1 ? maxBdp : pairBdp[n.Get(flow_input.src)][n.Get(flow_input.dst)]) : 0, global_t == 1 ? maxRtt : pairRtt[flow_input.src][flow_input.dst], Simulator::GetMaximumSimulationTime());
		//RdmaClientHelper clientHelper(flow_input.pg, serverAddress[flow_input.src], serverAddress[flow_input.dst], port, flow_input.dport, flow_input.maxPacketCount, 3000, global_t == 1 ? maxRtt : pairRtt[flow_input.src][flow_input.dst], Simulator::GetMaximumSimulationTime());
		ApplicationContainer appCon = clientHelper.Install(n.Get(flow_input.src));
//		appCon.Start(Seconds(flow_input.start_time));
		appCon.Start(Seconds(0)); // setting the correct time here conflicts with Sim time since there is already a schedule event that triggered this function at desired time.
		// get the next flow input
		flow_input.idx++;
		ReadFlowInput();
	}

	// schedule the next time to run this function
	if (flow_input.idx < flow_num) {
		Simulator::Schedule(Seconds(flow_input.start_time) - Simulator::Now(), ScheduleFlowInputs);
	} else { // no more flows, close the file
		flowf.close();
	}
}

void ScheduleInnerFlow() {
	uint32_t flows_cnt = 0;
	while (flows_cnt < flow_input.flows_rand_inner) 
	{
		uint32_t src = flow_input.inner_src_begin + ((double)flow_input.inner_src_end - flow_input.inner_src_begin) * rand () / RAND_MAX;
		uint32_t dst = flow_input.inner_dst_begin + ((double)flow_input.inner_dst_end - flow_input.inner_dst_begin) * rand () / RAND_MAX;

		/*Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
		uv->SetAttribute("Min", DoubleValue(flow_input.inner_src_begin));
		uv->SetAttribute("Max", DoubleValue(flow_input.inner_src_end));
		uint32_t src =  int(uv->GetValue());

		uv->SetAttribute("Min", DoubleValue( flow_input.inner_dst_begin));
		uv->SetAttribute("Max", DoubleValue(flow_input.inner_dst_end));
		uint32_t dst =  int(uv->GetValue());*/


		if (src == dst) 
			continue;
		
		uint32_t mid = flow_input.inner_src_begin + (flow_input.inner_src_end - flow_input.inner_src_begin + 1) / 2;
		if (!((src >= flow_input.inner_src_begin && src < mid && dst >= flow_input.inner_src_begin && dst < mid )//dc内主机
			|| (src >= mid && src < flow_input.inner_src_end && dst >= mid && dst < flow_input.inner_src_end)))
			continue;
		

		if (flow_rand_set.find(src<< 32 | dst) != flow_rand_set.end() )
			continue;
		flow_rand_set.insert(src<< 32 | dst);
		flows_cnt++;
		flow_input.idx++;
		flow_input.src = src;
		flow_input.dst = dst;
		flow_input.port = 10000+flow_input.idx;
		flow_input.dport = 20000+flow_input.idx;
		flows.push_back(flow_input);
		RdmaClientHelper clientHelper(flow_input.pg, serverAddress[flow_input.src], serverAddress[flow_input.dst], flow_input.port, flow_input.dport, flow_input.maxPacketCount, has_win ? (global_t == 1 ? maxBdp : pairBdp[n.Get(flow_input.src)][n.Get(flow_input.dst)]) : 0, global_t == 1 ? maxRtt : pairRtt[flow_input.src][flow_input.dst], Simulator::GetMaximumSimulationTime());
		ApplicationContainer appCon = clientHelper.Install(n.Get(flow_input.src));
		appCon.Start(Seconds(0)); // setting the correct time here conflicts with Sim time since there is already a schedule event that triggered this function at desired time.
	}
}

void ScheduleInterFlow() {
	uint32_t flows_cnt = 0;
	while (flows_cnt < flow_input.flows_rand_inter) 
	{
		uint32_t src = flow_input.inter_src_begin + ((double)flow_input.inter_src_end - flow_input.inter_src_begin) * rand () / RAND_MAX;
		uint32_t dst = flow_input.inter_dst_begin + ((double)flow_input.inter_dst_end - flow_input.inter_dst_begin) * rand () / RAND_MAX;

		/*Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
		uv->SetAttribute("Min", DoubleValue(flow_input.inter_src_begin));
		uv->SetAttribute("Max", DoubleValue(flow_input.inter_src_end));
        uint32_t src =  int(uv->GetValue());

		uv->SetAttribute("Min", DoubleValue( flow_input.inter_dst_begin));
		uv->SetAttribute("Max", DoubleValue(flow_input.inter_dst_end));
        uint32_t dst =  int(uv->GetValue());*/

		if (src == dst) 
			continue;
	

		if (flow_rand_set.find(src<< 32 | dst) != flow_rand_set.end() )
			continue;
		flow_rand_set.insert(src<< 32 | dst);
		flows_cnt++;
		flow_input.idx++;
		flow_input.src = src;
		flow_input.dst = dst;
		flow_input.port = 10000+flow_input.idx;
		flow_input.dport = 20000+flow_input.idx;
		flows.push_back(flow_input);
		RdmaClientHelper clientHelper(flow_input.pg, serverAddress[flow_input.src], serverAddress[flow_input.dst], flow_input.port, flow_input.dport, flow_input.maxPacketCount, has_win ? (global_t == 1 ? maxBdp : pairBdp[n.Get(flow_input.src)][n.Get(flow_input.dst)]) : 0, global_t == 1 ? maxRtt : pairRtt[flow_input.src][flow_input.dst], Simulator::GetMaximumSimulationTime());
		ApplicationContainer appCon = clientHelper.Install(n.Get(flow_input.src));
		appCon.Start(Seconds(0)); // setting the correct time here conflicts with Sim time since there is already a schedule event that triggered this function at desired time.
	}
}


Ipv4Address node_id_to_ip(uint32_t id) {
	return Ipv4Address(0x0b000001 + ((id / 256) * 0x00010000) + ((id % 256) * 0x00000100));
}

uint32_t ip_to_node_id(Ipv4Address ip) {
	return (ip.Get() >> 8) & 0xffff;
}

void qp_finish(FILE* fout, Ptr<RdmaQueuePair> q) {
	uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
	uint64_t base_rtt = pairRtt[sid][did], b = pairBw[sid][did];
	uint32_t total_bytes = q->m_size + ((q->m_size - 1) / packet_payload_size + 1) * (CustomHeader::GetStaticWholeHeaderSize() - IntHeader::GetStaticSize()); // translate to the minimum bytes required (with header but no INT)
	uint64_t standalone_fct = base_rtt + total_bytes * 8000000000lu / b;
	// sip, dip, sport, dport, size (B), start_time, fct (ns), standalone_fct (ns)
	fprintf(fout, "%08x %08x %u %u %lu %f %f %lu\n", q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->m_size, q->startTime.GetSeconds(), (Simulator::Now() - q->startTime).GetSeconds(), standalone_fct);
	fflush(fout);

	// ack代理模式下会提前qpfinish，此时能删除接收端不能接收报文了。
	// remove rxQp from the receiver
	/*Ptr<Node> dstNode = n.Get(did);
	Ptr<RdmaDriver> rdma = dstNode->GetObject<RdmaDriver> ();
	rdma->m_rdma->DeleteRxQp(q->sip.Get(), q->m_pg, q->sport);*/ 
}

void get_pfc(FILE* fout, Ptr<QbbNetDevice> dev, uint32_t type) {
	fprintf(fout, "%f %u %u %u %u\n", Simulator::Now().GetSeconds(), dev->GetNode()->GetId(), dev->GetNode()->GetNodeType(), dev->GetIfIndex(), type);
}

struct QlenDistribution {
	vector<uint32_t> cnt; // cnt[i] is the number of times that the queue len is i KB

	void add(uint32_t qlen) {
		uint32_t kb = qlen / 1000;
		if (cnt.size() < kb + 1)
			cnt.resize(kb + 1);
		cnt[kb]++;
	}
};
map<uint32_t, map<uint32_t, QlenDistribution> > queue_result;
void monitor_buffer(FILE* qlen_output, NodeContainer *n) {
	for (uint32_t i = 0; i < n->GetN(); i++) {
		if (n->Get(i)->GetNodeType() == 1) { // is switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n->Get(i));
			if (queue_result.find(i) == queue_result.end())
				queue_result[i];
			for (uint32_t j = 1; j < sw->GetNDevices(); j++) {
				uint32_t size = 0;
				for (uint32_t k = 0; k < SwitchMmu::qCnt; k++)
					size += sw->m_mmu->egress_bytes[j][k];
				queue_result[i][j].add(size);
			}
		}
	}
	if (Simulator::Now().GetTimeStep() % qlen_dump_interval == 0) {
		fprintf(qlen_output, "time: %lu\n", Simulator::Now().GetTimeStep());
		for (auto &it0 : queue_result)
			for (auto &it1 : it0.second) {
				fprintf(qlen_output, "%u %u", it0.first, it1.first);
				auto &dist = it1.second.cnt;
				for (uint32_t i = 0; i < dist.size(); i++)
					fprintf(qlen_output, " %u", dist[i]);
				fprintf(qlen_output, "\n");
			}
		fflush(qlen_output);
	}
	if (Simulator::Now().GetTimeStep() < qlen_mon_end)
		Simulator::Schedule(NanoSeconds(qlen_mon_interval), &monitor_buffer, qlen_output, n);
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
			// skip down link
			if (!it->second.up)
				continue;
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
			Ipv4Address dstAddr = dst->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
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

// take down the link between a and b, and redo the routing
void TakeDownLink(NodeContainer n, Ptr<Node> a, Ptr<Node> b) {
	if (!nbr2if[a][b].up)
		return;
	// take down link between a and b
	nbr2if[a][b].up = nbr2if[b][a].up = false;
	nextHop.clear();
	CalculateRoutes(n);
	// clear routing tables
	for (uint32_t i = 0; i < n.GetN(); i++) {
		if (n.Get(i)->GetNodeType() == 1)
			DynamicCast<SwitchNode>(n.Get(i))->ClearTable();
		else
			n.Get(i)->GetObject<RdmaDriver>()->m_rdma->ClearTable();
	}
	DynamicCast<QbbNetDevice>(a->GetDevice(nbr2if[a][b].idx))->TakeDown();
	DynamicCast<QbbNetDevice>(b->GetDevice(nbr2if[b][a].idx))->TakeDown();
	// reset routing table
	SetRoutingEntries();

	// redistribute qp on each host
	for (uint32_t i = 0; i < n.GetN(); i++) {
		if (n.Get(i)->GetNodeType() == 0)
			n.Get(i)->GetObject<RdmaDriver>()->m_rdma->RedistributeQp();
	}
}

uint64_t get_nic_rate(NodeContainer &n) {
	for (uint32_t i = 0; i < n.GetN(); i++)
		if (n.Get(i)->GetNodeType() == 0)
			return DynamicCast<QbbNetDevice>(n.Get(i)->GetDevice(1))->GetDataRate().GetBitRate();
}


std::unordered_set<std::string> torIngressStatis;
std::unordered_set<std::string> torEgressStatis;
void PrintResults(NodeContainer ToR, uint32_t numToRs, double delay) {
	for (uint32_t i = 0; i < numToRs; i++) {
		double throughputTotal = 0;
		uint64_t torBuffer = 0;
		double power;
		Ptr<SwitchNode> node = DynamicCast<SwitchNode>(ToR.Get (i));
		
		for (uint32_t j = 0; j < node->GetNDevices()-1; j++) {
			if (j > 16 && j < 32) 
			{
				continue;
			}
			Ptr<QbbNetDevice> nd = DynamicCast<QbbNetDevice>(node->GetDevice(j+1));
//			uint64_t txBytes = nd->getTxBytes();
			uint64_t txBytes = nd->GetQueue()->getTxBytes();
			double rxBytes = nd->getNumRxBytes();
			uint64_t rxIngressBytes = nd->getRxBytes();

			uint64_t qlen = nd->GetQueue()->GetNBytesTotal();
			uint64_t bw = nd->GetDataRate().GetBitRate(); //maxRtt

			torBuffer += qlen;
			double throughput = double(txBytes * 8) / delay;
			if (throughput > 1e11)//超过100Gbps，需要计算偏移
			{
				double Offset = (1.009+ ((int)(throughput/1e11-1))*0.0125);
				throughput = throughput / Offset;
			}
			double rxThroughput = double(rxIngressBytes * 8) / delay;
			if (rxThroughput > 1e11)//超过100Gbps，需要计算偏移
			{
				double Offset = (1.009+ ((int)(rxThroughput/1e11-1))*0.012);
				rxThroughput = rxThroughput / Offset;
			}
			if (j == 16) { //  ToDo. very ugly hardcode here specific to the burst evaluation scenario where 16 is the receiver in flow-burstExp.txt.
				throughputTotal += throughput;
				power = (rxBytes * 8.0 / delay) * (qlen + bw * maxRtt * 1e-9) / (bw * (bw * maxRtt * 1e-9));

			}

			uint32_t inqlen = 0;
			uint32_t eqlen = 0;
			for (uint32_t q = 0; q < node->m_mmu->qCnt; q++) {
				inqlen += node->m_mmu->ingress_bytes[j+1][q];
				eqlen += node->m_mmu->egress_bytes[j+1][q];
			}
			PrintCnt++;
			std::string torName = std::to_string(i) + "-" + std::to_string(j);
			bool egressPrint = false;
			bool ingressPrint = false;
			if (torIngressStatis.find(torName) == torIngressStatis.end())
			{
				if (rxIngressBytes > 0)
				{
					torIngressStatis.insert(torName);
					ingressPrint = true;
				}
			}
			else
			{
				ingressPrint = true;
			}

			if (torEgressStatis.find(torName) == torEgressStatis.end())
			{
				if (txBytes > 0)
				{
					torEgressStatis.insert(torName);
					egressPrint = true;
				}
			}
			else
			{
				egressPrint = true;
			}

			if (ingressPrint )
				std::cout << "ToRIngress " << i << " Port " << j  << " rxTh " << rxThroughput << " rxBytes " << rxIngressBytes  << " inQlen " << inqlen   <<  " time " << Simulator::Now().GetSeconds()<< std::endl;
			if ( egressPrint)
				std::cout << "ToREgress " << i << " Port " << j << " throughput " << throughput << " txBytes " << txBytes << " qlen " << qlen  << " eQlen " << eqlen   << " normpower " << power << " time " << Simulator::Now().GetSeconds() << std::endl;
		}
		PrintCnt++;
		//std::cout << "ToRTotal " << i << " Total " << 0 << " throughput " << throughputTotal << " buffer " << torBuffer <<  " time " << Simulator::Now().GetSeconds() << std::endl;
	}

	int finishCnt = 0;
	for (auto it = flows.begin(); it != flows.end(); it++) {
		FlowInput flow_input = *it;
		Ptr<Node> dstNode = n.Get(flow_input.dst);
		Ptr<RdmaDriver> rdma = dstNode->GetObject<RdmaDriver> ();
		Ptr<RdmaRxQueuePair> rxQp = rdma->m_rdma->GetRxQp(serverAddress[flow_input.dst].Get(), serverAddress[flow_input.src].Get(),  flow_input.dport, flow_input.port, flow_input.pg, false);
		if (rxQp != NULL)
		{
			if (rxQp->ReceiverNextExpectedSeq >= flow_input.maxPacketCount) //接收端接收完成
			{
				finishCnt +=1;
				continue;
			}

			uint64_t key = ((uint64_t)serverAddress[flow_input.src].Get() << 32) | ((uint64_t)flow_input.pg << 16) | (uint64_t)flow_input.port;
			uint64_t last_recv_size;
			
			if (qpRecvSize.find(key) == qpRecvSize.end())
			{
				last_recv_size = 0;
			}
			else
			{
				last_recv_size = qpRecvSize[key];
			}
			qpRecvSize[key] = rxQp->m_recv_size;

			double throughput =0;
			if (rxQp->m_recv_size >= last_recv_size)
			{
                throughput = double((rxQp->m_recv_size - last_recv_size) * 8) / delay;
				if (throughput > 1e11)//超过100Gbps，需要计算偏移
				{
					double Offset = (1.009+ ((int)(throughput/1e11-1))*0.01275);
					throughput = throughput / Offset;
				}
            }
				
            if ((throughput > (9e11+1e-6)) || (throughput < (0-1e-6)))
            {
                throughput =0;
                std::cout << "throughput error: Src " << flow_input.src << " Dst " << flow_input.dst << " throughput " << throughput <<   " time " << Simulator::Now().GetSeconds() <<  " m_recv_size " << rxQp->m_recv_size << " last_recv_size " << last_recv_size << " delay " << delay  << std::endl;
            }
			PrintCnt++;
            std::cout << "Flow: Src " << flow_input.src << " Dst " << flow_input.dst << " dport " << flow_input.dport  << " throughput " << throughput <<  " time " << Simulator::Now().GetSeconds() <<  " m_recv_size " << rxQp->m_recv_size << std::endl;
		}
	}

	for (uint32_t i = 0; i < numToRs; i++) {
		Ptr<SwitchNode> node = DynamicCast<SwitchNode>(ToR.Get (i));
		if (node != NULL)
		{
			if (node->m_wRdmaGateway.m_gatewayType != 0)
			{
				std::cout << "Gateway " << i << " gwMirrorPoolUsed(MB) " <<  (double)node->m_wRdmaGateway.m_mirrorPool.wGwMirrorPoolUsed()/(1024*1024) << " gwFlowPoolUsed(MB) " << (double)node->m_wRdmaGateway.m_flowPool.wGwGetFlowPoolUsed()/(1024*1024) <<  " time " << Simulator::Now().GetSeconds() << std::endl;
			}
		}
	}

	if (finishCnt >= flow_num || (simulator_stop_one_flow && finishCnt >= 1))
	{
		PrintCnt++;
		std::cout << "All flows finish at time " << Simulator::Now().GetSeconds() << std::endl;
		for (uint32_t i = 0; i < numToRs; i++) {
			Ptr<SwitchNode> node = DynamicCast<SwitchNode>(ToR.Get (i));
			if (node != NULL)
			{
				if (node->m_wRdmaGateway.m_gatewayType != 0)
				{
					std::cout << "Gateway " << i << " MaxMirrorPoolUsed(MB) " <<  (double)node->m_wRdmaGateway.m_mirrorPool.wGwMirrorMaxPoolUsed()/(1024*1024) << " MaxFlowPoolUsed(MB) " << (double)node->m_wRdmaGateway.m_flowPool.wGwGetFlowMaxPoolUsed()/(1024*1024) <<  " time " << Simulator::Now().GetSeconds() << std::endl;
				}

				if (node->m_atcGateway.m_gatewayType != 0)
				{
					std::cout << "Gateway " << i << " maxVoqBytes(MB) " <<  (double)node->m_atcGateway.longHaulState.maxVoqBytes/(1024*1024) << " maxTotalVoqBytes(MB) " << (double)node->m_atcGateway.longHaulState.maxTotalVoqBytes/(1024*1024) <<  " time " << Simulator::Now().GetSeconds() << std::endl;
				}

				if (node->m_lr2Gateway.m_gatewayType != 0)
				{
					std::cout << "Gateway " << i << " MaxReorderingPoolUsed(MB) " <<  (double)node->m_lr2Gateway.m_maxReorderingPoolBytes/(1024*1024) << " MaxBackupPoolUsed(MB) " << (double)node->m_lr2Gateway.m_maxBackupPoolBytes/(1024*1024) <<  " time " << Simulator::Now().GetSeconds() << std::endl;
				}
			}
		}
		Simulator::Stop(Seconds(0.01));
		return;
	}

	if(PrintCnt > MAX_PRINT_CNT)
	{
		std::cout << "++++too mush PrintCnt " << PrintCnt << " time " << Simulator::Now().GetSeconds() << std::endl;
		return;
	}

	Simulator::Schedule(Seconds(delay), PrintResults, ToR, numToRs, delay);
}


void PrintResultsFlow(std::map<uint32_t, NetDeviceContainer> Src, uint32_t numFlows, double delay) {
	for (uint32_t i = 0; i < numFlows; i++) {
		double throughputTotal = 0;

		for (uint32_t j = 0; j < Src[i].GetN(); j++) {
			Ptr<QbbNetDevice> nd = DynamicCast<QbbNetDevice>(Src[i].Get(j));
//			uint64_t txBytes = nd->getTxBytes();
			uint64_t txBytes = nd->getNumTxBytes();

			uint64_t qlen = nd->GetQueue()->GetNBytesTotal();
			double throughput = double(txBytes * 8) / delay;
			throughputTotal += throughput;
			// std::cout << "Src " << i << " Port " << j << " throughput "<< throughput << " txBytes " << txBytes << " qlen " << qlen << " time " << Simulator::Now().GetSeconds() << std::endl;
		}
		std::cout << "Src " << i << " Total " << 0 << " throughput " << throughputTotal <<  " time " << Simulator::Now().GetSeconds() << std::endl;
	}
	Simulator::Schedule(Seconds(delay), PrintResultsFlow, Src, numFlows, delay);
}




int main(int argc, char *argv[])
{
	clock_t begint, endt;
	begint = clock();
	std::ifstream conf;
	bool wien = false;//true; // wien enables PowerTCP. 
	bool delayWien = false; // delayWien enables Theta-PowerTCP (delaypowertcp) 

	uint32_t algorithm = 3;
	uint32_t windowCheck = 1;
	std::string confFile = "/home/hhq/Desktop/ns3-datacenter/simulator/ns-3.39/examples/PowerTCP/config-gateway.txt";
	std::cout << confFile;
	CommandLine cmd;
	cmd.AddValue("conf", "config file path", confFile);
	cmd.AddValue("wien", "enable wien --> wien enables PowerTCP.", wien);
	cmd.AddValue("delayWien", "enable wien delay --> delayWien enables Theta-PowerTCP (delaypowertcp) ", delayWien);

	cmd.AddValue ("algorithm", "specify CC mode. This is added for my convinience. I prefer cmd rather than parsing files.", algorithm);
	cmd.AddValue("windowCheck", "windowCheck", windowCheck);
	cmd.Parse (argc, argv);
	conf.open(confFile.c_str());
	while (!conf.eof())
	{
		std::string key;
		conf >> key;
		std::cout << key;
		if (key.compare("ENABLE_QCN") == 0)
		{
			uint32_t v;
			conf >> v;
			enable_qcn = v;
			if (enable_qcn)
				std::cout << "ENABLE_QCN\t\t\t" << "Yes" << "\n";
			else
				std::cout << "ENABLE_QCN\t\t\t" << "No" << "\n";
		}
		else if (key.compare("SIMULATOR_STOP_ONE_FLOW") == 0)
		{
			uint32_t v;
			conf >> v;
			simulator_stop_one_flow = v;
			if (simulator_stop_one_flow)
				std::cout << "SIMULATOR_STOP_ONE_FLOW\t\t" << "Yes" << "\n";
			else
				std::cout << "SIMULATOR_STOP_ONE_FLOW\t\t" << "No" << "\n";
		}
		else if (key.compare("LOG_INTERVAL_US") == 0)
		{
			uint32_t v;
			conf >> v;
			LogIntervalUs = v;
			std::cout << "LOG_INTERVAL_US\t\t" << LogIntervalUs << "\n";

		}
		else if (key.compare("CLAMP_TARGET_RATE") == 0)
		{
			uint32_t v;
			conf >> v;
			clamp_target_rate = v;
			if (clamp_target_rate)
				std::cout << "CLAMP_TARGET_RATE\t\t" << "Yes" << "\n";
			else
				std::cout << "CLAMP_TARGET_RATE\t\t" << "No" << "\n";
		}
		else if (key.compare("ByteCounterEnabled") == 0)
		{
			uint32_t v;
			conf >> v;
			byteCounterEnabled = v;
			if (byteCounterEnabled)
				std::cout << "ByteCounterEnabled\t\t" << "Yes" << "\n";
			else
				std::cout << "ByteCounterEnabled\t\t" << "No" << "\n";
		}
		else if (key.compare("ByteCounterThreshold") == 0)
		{
			uint64_t v;
			conf >> v;
			byteCounterThreshold = v;
			std::cout << "byteCounterThreshold\t\t\t" << byteCounterThreshold << "\n";
		}
		else if (key.compare("PAUSE_TIME") == 0)
		{
			double v;
			conf >> v;
			pause_time = v;
			std::cout << "PAUSE_TIME\t\t\t" << pause_time << "\n";
		}
		else if (key.compare("BIFROST_TIME") == 0)
		{
			uint32_t v;
			conf >> v;
			bifrostTime = v;
			std::cout << "BIFROST_TIME\t\t" << bifrostTime << "\n";
		}
		else if (key.compare("BIFROST_K") == 0)
		{
			uint32_t v;
			conf >> v;
			bifrostK = v;
			std::cout << "BIFROST_K\t\t" << bifrostK << "\n";
		}
		else if (key.compare("BIFROST_LINKS") == 0) {
			int n_links ;
			conf >> n_links;
			std::cout << "BIFROST_LINKS\t\t\t\t";
			for (int i = 0; i < n_links; i++) {
				uint32_t srcNodeId;
				uint32_t dstNodeId;
				conf >> srcNodeId >> dstNodeId;
				bifrostLinks[srcNodeId] = dstNodeId;
				bifrostFlag = true;
				std::cout << ' ' << srcNodeId << ' ' << dstNodeId;
			}
			std::cout << '\n';
		}
		else if (key.compare("GATEWAY_TYPE") == 0)
		{
			uint32_t v;
			conf >> v;
			gatewayFlag = v;
			std::cout << "GATEWAY_TYPE\t\t" << gatewayFlag << "\n";
		}
		else if (key.compare("GATEWAY_WIN") == 0)
		{
			uint32_t v;
			conf >> v;
			gatewayWin = v;
			std::cout << "GATEWAY_WIN\t\t" << gatewayWin << "\n";
		}
		else if (key.compare("GATEWAY_L2TIMEOUT") == 0)
		{
			uint32_t v;
			conf >> v;
			gatewayL2Timeout = v;
			std::cout << "GATEWAY_L2TIMEOUT\t\t" << gatewayL2Timeout << "\n";
		}
		else if (key.compare("GATEWAY_SRC_POOLSIZE") == 0)
		{
			uint32_t v;
			conf >> v;
			gatewaySrcPoolSize = v;
			std::cout << "GATEWAY_SRC_POOLSIZE\t\t" << gatewaySrcPoolSize << "\n";
		}
		else if (key.compare("GATEWAY_DST_POOLSIZE") == 0)
		{
			uint32_t v;
			conf >> v;
			gatewayDstPoolSize = v;
			std::cout << "GATEWAY_DST_POOLSIZE\t\t" << gatewayDstPoolSize << "\n";
		}
		else if (key.compare("GATEWAY_SRC") == 0) {
			int n_nodes ;
			conf >> n_nodes;
			std::cout << "GATEWAY_SRC\t\t\t\t";
			for (int i = 0; i < n_nodes; i++) {
				uint32_t nodeId;
				conf >> nodeId ;
				auto it = gatewayNodeType.find(nodeId);
				if (it == gatewayNodeType.end())
				{
					gatewayNodeType[nodeId] = 1;
				}
				else
				{
					gatewayNodeType[nodeId] = it->second | 1;
				}
				
				std::cout << ' ' << nodeId;
			}
			std::cout << '\n';
		}
		else if (key.compare("GATEWAY_DST") == 0) {
			int n_nodes ;
			conf >> n_nodes;
			std::cout << "GATEWAY_DST\t\t\t\t";
			for (int i = 0; i < n_nodes; i++) {
				uint32_t nodeId;
				conf >> nodeId ;
				auto it = gatewayNodeType.find(nodeId);
				if (it == gatewayNodeType.end())
				{
					gatewayNodeType[nodeId] = 2;
				}
				else
				{
					gatewayNodeType[nodeId] = it->second | 2;
				}
				std::cout << ' ' << nodeId;
			}
			std::cout << '\n';
		}
		else if (key.compare("GATEWAY_PEER_OUTPORT_RATE") == 0)
		{
			uint64_t v;
			conf >> v;
			gatewayPeerOutputRate = v;
			std::cout << "GATEWAY_PEER_OUTPORT_RATE\t\t" << gatewayPeerOutputRate << "\n";
		}
		else if (key.compare("GATEWAY_XOR_ENABLE") == 0)
		{
			uint32_t v;
			conf >> v;
			gatewayXorEnable = v;
			if (gatewayXorEnable)
				std::cout << "GATEWAY_XOR_ENABLE\t\t" << "Yes" << "\n";
			else
				std::cout << "GATEWAY_XOR_ENABLE\t\t" << "No" << "\n";
		}
		else if (key.compare("GATEWAY_SR_ENABLE") == 0)
		{
			uint32_t v;
			conf >> v;
			gatewaySrEnable = v;
			if (gatewaySrEnable)
				std::cout << "GATEWAY_SR_ENABLE\t\t" << "Yes" << "\n";
			else
				std::cout << "GATEWAY_SR_ENABLE\t\t" << "No" << "\n";
		}

		else if (key.compare("MAX_ATC_VOQ_COUNT") == 0)
		{
			uint64_t v;
			conf >> v;
			max_voq_count = v;
			std::cout << "MAX_ATC_VOQ_COUNT\t\t" << max_voq_count << "\n";
		}
		else if (key.compare("MAX_ATC_VOQ_RATE") == 0)
		{
			uint64_t v;
			conf >> v;
			max_voq_rate = v;
			std::cout << "MAX_ATC_VOQ_RATE\t\t" << max_voq_rate << "\n";
		}
		// LR2网关配置参数
		else if (key.compare("LR2_DEPOT_REORDERING_SIZE") == 0)
		{
			uint64_t v;
			conf >> v;
			lr2DepotReorderingSize = v;
			std::cout << "LR2_DEPOT_REORDERING_SIZE\t\t" << lr2DepotReorderingSize << "\n";
		}
		else if (key.compare("LR2_DEPOT_BACKUP_SIZE") == 0)
		{
			uint64_t v;
			conf >> v;
			lr2DepotBackupSize = v;
			std::cout << "LR2_DEPOT_BACKUP_SIZE\t\t" << lr2DepotBackupSize << "\n";
		}
		else if (key.compare("LR2_DEPOT_BACKUP_TIMEOUT") == 0)
		{
			uint32_t v;
			conf >> v;
			lr2DepotBackupTimeout = v;
			std::cout << "LR2_DEPOT_BACKUP_TIMEOUT\t\t" << lr2DepotBackupTimeout << "\n";
		}
		else if (key.compare("FLOWS_RAND_INNER") == 0) {
			uint32_t flows ;
			conf >> flows;
			flow_input.flows_rand_inner = flows;
			if (flows != 0)
			{
				std::string src_flows;
				std::string dst_flows;
				conf >> src_flows;
				sscanf(src_flows.c_str(), "%d-%d", &flow_input.inner_src_begin, &flow_input.inner_src_end);
				conf >> dst_flows;
				sscanf(dst_flows.c_str(), "%d-%d", &flow_input.inner_dst_begin, &flow_input.inner_dst_end);
			}

			std::cout << "FLOWS_RAND_INNER flows: " << flow_input.flows_rand_inner  << ", SRC: " << flow_input.inner_src_begin << '-' << flow_input.inner_src_end <<  ",DST:" << flow_input.inner_dst_begin << '-' << flow_input.inner_dst_end << '\n';
		}
		else if (key.compare("FLOWS_RAND_INTER") == 0) {
			uint32_t flows ;
			conf >> flows;
			flow_input.flows_rand_inter = flows;
			if (flows != 0)
			{
				std::string src_flows;
				std::string dst_flows;
				conf >> src_flows;
				sscanf(src_flows.c_str(), "%d-%d", &flow_input.inter_src_begin, &flow_input.inter_src_end);
				conf >> dst_flows;
				sscanf(dst_flows.c_str(), "%d-%d", &flow_input.inter_dst_begin, &flow_input.inter_dst_end);
			}

			std::cout << "FLOWS_RAND_INTER flows: " << flow_input.flows_rand_inter  << ", SRC: " << flow_input.inter_src_begin << '-' << flow_input.inter_src_end <<  ",DST:" << flow_input.inter_dst_begin << '-' << flow_input.inter_dst_end << '\n';
		}
		else if (key.compare("FLOWS_RAND_SIZE") == 0)
		{
			uint64_t v;
			conf >> v;
			flow_input.flows_rand_packets = v;
			std::cout << "FLOWS_RAND_SIZE\t\t" << flow_input.flows_rand_packets << "\n";
		}
		else if (key.compare("FLOWS_RAND_START_TIME") == 0)
		{
			double v;
			conf >> v;
			flow_input.flows_rand_start_time = v;
			std::cout << "FLOWS_RAND_START_TIME\t\t" << flow_input.flows_rand_start_time << "\n";
		}
		else if (key.compare("ENABLE_PFC") == 0)
		{
			uint32_t v;
			conf >> v;
			enable_pfc = v;
			if (enable_pfc)
				std::cout << "ENABLE_PFC\t\t\t" << "Yes" << "\n";
			else
				std::cout << "ENABLE_PFC\t\t\t" << "No" << "\n";
		}
		else if (key.compare("ENABLE_IRN") == 0)
		{
			uint32_t v;
			conf >> v;
			enable_irn = v;
			if (enable_irn)
				std::cout << "ENABLE_IRN\t\t\t" << "Yes" << "\n";
			else
				std::cout << "ENABLE_IRN\t\t\t" << "No" << "\n";
		}
		else if (key.compare("IRN_BDP") == 0)
		{
			uint32_t v;
			conf >> v;
			irn_bdp = v;
			std::cout << "IRN_BDP\t\t" << irn_bdp << "\n";
		}
		else if (key.compare("IRN_MIN_RTO") == 0)
		{
			std::string v;
			conf >> v;
			irn_rto_min = v;
			std::cout << "IRN_MIN_RTO\t\t\t" << irn_rto_min << "\n";
		}
		else if (key.compare("IRN_MAX_RTO") == 0)
		{
			std::string v;
			conf >> v;
			irn_rto_max = v;
			std::cout << "IRN_MAX_RTO\t\t\t" << irn_rto_max << "\n";
		}
		else if (key.compare("L2TIMEOUT") == 0)
		{
			uint32_t v;
			conf >> v;
			l2timeout = v;
			std::cout << "L2TIMEOUT\t\t" << l2timeout << "\n";
		}
		else if (key.compare("DATA_RATE") == 0)
		{
			std::string v;
			conf >> v;
			data_rate = v;
			std::cout << "DATA_RATE\t\t\t" << data_rate << "\n";
		}
		else if (key.compare("LINK_DELAY") == 0)
		{
			std::string v;
			conf >> v;
			link_delay = v;
			std::cout << "LINK_DELAY\t\t\t" << link_delay << "\n";
		}
		else if (key.compare("PACKET_PAYLOAD_SIZE") == 0)
		{
			uint32_t v;
			conf >> v;
			packet_payload_size = v;
			std::cout << "PACKET_PAYLOAD_SIZE\t\t" << packet_payload_size << "\n";
		}
		else if (key.compare("L2_CHUNK_SIZE") == 0)
		{
			uint32_t v;
			conf >> v;
			l2_chunk_size = v;
			std::cout << "L2_CHUNK_SIZE\t\t\t" << l2_chunk_size << "\n";
		}
		else if (key.compare("L2_ACK_INTERVAL") == 0)
		{
			uint32_t v;
			conf >> v;
			l2_ack_interval = v;
			std::cout << "L2_ACK_INTERVAL\t\t\t" << l2_ack_interval << "\n";
		}
		else if (key.compare("CNP_GENERATION_INTERVAL") == 0)
		{
			uint32_t v;
			conf >> v;
			cnpGenerationInterval = v;
			std::cout << "CNP_GENERATION_INTERVAL\t\t\t" << cnpGenerationInterval << "\n";
		}
		else if (key.compare("L2_BACK_TO_ZERO") == 0)
		{
			uint32_t v;
			conf >> v;
			l2_back_to_zero = v;
			if (l2_back_to_zero)
				std::cout << "L2_BACK_TO_ZERO\t\t\t" << "Yes" << "\n";
			else
				std::cout << "L2_BACK_TO_ZERO\t\t\t" << "No" << "\n";
		}
		else if (key.compare("TOPOLOGY_FILE") == 0)
		{
			std::string v;
			conf >> v;
			topology_file = v;
			std::cout << "TOPOLOGY_FILE\t\t\t" << topology_file << "\n";
		}
		else if (key.compare("FLOW_FILE") == 0)
		{
			std::string v;
			conf >> v;
			flow_file = v;
			std::cout << "FLOW_FILE\t\t\t" << flow_file << "\n";
		}
		else if (key.compare("TRACE_FILE") == 0)
		{
			std::string v;
			conf >> v;
			trace_file = v;
			std::cout << "TRACE_FILE\t\t\t" << trace_file << "\n";
		}
		else if (key.compare("TRACE_OUTPUT_FILE") == 0)
		{
			std::string v;
			conf >> v;
			trace_output_file = v;
			if (argc > 2)
			{
				trace_output_file = trace_output_file + std::string(argv[2]);
			}
			std::cout << "TRACE_OUTPUT_FILE\t\t" << trace_output_file << "\n";
		}
		else if (key.compare("SIMULATOR_STOP_TIME") == 0)
		{
			double v;
			conf >> v;
			simulator_stop_time = v;
			std::cout << "SIMULATOR_STOP_TIME\t\t" << simulator_stop_time << "\n";
		}
		else if (key.compare("ALPHA_RESUME_INTERVAL") == 0)
		{
			double v;
			conf >> v;
			alpha_resume_interval = v;
			std::cout << "ALPHA_RESUME_INTERVAL\t\t" << alpha_resume_interval << "\n";
		}
		else if (key.compare("RP_TIMER") == 0)
		{
			double v;
			conf >> v;
			rp_timer = v;
			std::cout << "RP_TIMER\t\t\t" << rp_timer << "\n";
		}
		else if (key.compare("EWMA_GAIN") == 0)
		{
			double v;
			conf >> v;
			ewma_gain = v;
			std::cout << "EWMA_GAIN\t\t\t" << ewma_gain << "\n";
		}
		else if (key.compare("FAST_RECOVERY_TIMES") == 0)
		{
			uint32_t v;
			conf >> v;
			fast_recovery_times = v;
			std::cout << "FAST_RECOVERY_TIMES\t\t" << fast_recovery_times << "\n";
		}
		else if (key.compare("RATE_AI") == 0)
		{
			std::string v;
			conf >> v;
			rate_ai = v;
			std::cout << "RATE_AI\t\t\t\t" << rate_ai << "\n";
		}
		else if (key.compare("RATE_HAI") == 0)
		{
			std::string v;
			conf >> v;
			rate_hai = v;
			std::cout << "RATE_HAI\t\t\t" << rate_hai << "\n";
		}
		else if (key.compare("ERROR_RATE_PER_LINK") == 0)
		{
			double v;
			conf >> v;
			error_rate_per_link = v;
			std::cout << "ERROR_RATE_PER_LINK\t\t" << error_rate_per_link << "\n";
		}
		else if (key.compare("CC_MODE") == 0) {
			conf >> cc_mode;
			std::cout << "CC_MODE\t\t" << cc_mode << '\n';
		} else if (key.compare("RATE_DECREASE_INTERVAL") == 0) {
			double v;
			conf >> v;
			rate_decrease_interval = v;
			std::cout << "RATE_DECREASE_INTERVAL\t\t" << rate_decrease_interval << "\n";
		} else if (key.compare("MIN_RATE") == 0) {
			conf >> min_rate;
			std::cout << "MIN_RATE\t\t" << min_rate << "\n";
		} else if (key.compare("FCT_OUTPUT_FILE") == 0) {
			conf >> fct_output_file;
			std::cout << "FCT_OUTPUT_FILE\t\t" << fct_output_file << '\n';
		} else if (key.compare("HAS_WIN") == 0) {
			conf >> has_win;
			std::cout << "HAS_WIN\t\t" << has_win << "\n";
		} else if (key.compare("GLOBAL_T") == 0) {
			conf >> global_t;
			std::cout << "GLOBAL_T\t\t" << global_t << '\n';
		} else if (key.compare("MI_THRESH") == 0) {
			conf >> mi_thresh;
			std::cout << "MI_THRESH\t\t" << mi_thresh << '\n';
		} else if (key.compare("VAR_WIN") == 0) {
			uint32_t v;
			conf >> v;
			var_win = v;
			std::cout << "VAR_WIN\t\t" << v << '\n';
		} else if (key.compare("FAST_REACT") == 0) {
			uint32_t v;
			conf >> v;
			fast_react = v;
			std::cout << "FAST_REACT\t\t" << v << '\n';
		} else if (key.compare("U_TARGET") == 0) {
			conf >> u_target;
			std::cout << "U_TARGET\t\t" << u_target << '\n';
		} else if (key.compare("INT_MULTI") == 0) {
			conf >> int_multi;
			std::cout << "INT_MULTI\t\t\t\t" << int_multi << '\n';
		} else if (key.compare("RATE_BOUND") == 0) {
			uint32_t v;
			conf >> v;
			rate_bound = v;
			std::cout << "RATE_BOUND\t\t" << rate_bound << '\n';
		} else if (key.compare("ACK_HIGH_PRIO") == 0) {
			conf >> ack_high_prio;
			std::cout << "ACK_HIGH_PRIO\t\t" << ack_high_prio << '\n';
		} else if (key.compare("DCTCP_RATE_AI") == 0) {
			conf >> dctcp_rate_ai;
			std::cout << "DCTCP_RATE_AI\t\t\t\t" << dctcp_rate_ai << "\n";
		} else if (key.compare("PFC_OUTPUT_FILE") == 0) {
			conf >> pfc_output_file;
			std::cout << "PFC_OUTPUT_FILE\t\t\t\t" << pfc_output_file << '\n';
		} else if (key.compare("LINK_DOWN") == 0) {
			conf >> link_down_time >> link_down_A >> link_down_B;
			std::cout << "LINK_DOWN\t\t\t\t" << link_down_time << ' ' << link_down_A << ' ' << link_down_B << '\n';
		} else if (key.compare("ENABLE_TRACE") == 0) {
			conf >> enable_trace;
			std::cout << "ENABLE_TRACE\t\t\t\t" << enable_trace << '\n';
		} 
		else if (key.compare("KMAX_MAP") == 0) {
			int n_k ;
			conf >> n_k;
			std::cout << "KMAX_MAP\t\t\t\t";
			for (int i = 0; i < n_k; i++) {
				uint64_t rate;
				uint32_t k;
				conf >> rate >> k;
				rate2kmax[rate] = k;
				std::cout << ' ' << rate << ' ' << k;
			}
			std::cout << '\n';
		} else if (key.compare("KMIN_MAP") == 0) {
			int n_k ;
			conf >> n_k;
			std::cout << "KMIN_MAP\t\t\t\t";
			for (int i = 0; i < n_k; i++) {
				uint64_t rate;
				uint32_t k;
				conf >> rate >> k;
				rate2kmin[rate] = k;
				std::cout << ' ' << rate << ' ' << k;
			}
			std::cout << '\n';
		} else if (key.compare("PMAX_MAP") == 0) {
			int n_k ;
			conf >> n_k;
			std::cout << "PMAX_MAP\t\t\t\t";
			for (int i = 0; i < n_k; i++) {
				uint64_t rate;
				double p;
				conf >> rate >> p;
				rate2pmax[rate] = p;
				std::cout << ' ' << rate << ' ' << p;
			}
			std::cout << '\n';
		} else if (key.compare("BUFFER_SIZE") == 0) {
			conf >> buffer_size;
			std::cout << "BUFFER_SIZE\t\t\t\t" << buffer_size << '\n';
		} else if (key.compare("QLEN_MON_FILE") == 0) {
			conf >> qlen_mon_file;
			std::cout << "QLEN_MON_FILE\t\t\t\t" << qlen_mon_file << '\n';
		} else if (key.compare("QLEN_MON_START") == 0) {
			conf >> qlen_mon_start;
			std::cout << "QLEN_MON_START\t\t\t\t" << qlen_mon_start << '\n';
		} else if (key.compare("QLEN_MON_END") == 0) {
			conf >> qlen_mon_end;
			std::cout << "QLEN_MON_END\t\t\t\t" << qlen_mon_end << '\n';
		} else if (key.compare("MULTI_RATE") == 0) {
			int v;
			conf >> v;
			multi_rate = v;
			std::cout << "MULTI_RATE\t\t\t\t" << multi_rate << '\n';
		} else if (key.compare("SAMPLE_FEEDBACK") == 0) {
			int v;
			conf >> v;
			sample_feedback = v;
			std::cout << "SAMPLE_FEEDBACK\t\t\t\t" << sample_feedback << '\n';
		} else if (key.compare("PINT_LOG_BASE") == 0) {
			conf >> pint_log_base;
			std::cout << "PINT_LOG_BASE\t\t\t\t" << pint_log_base << '\n';
		} else if (key.compare("PINT_PROB") == 0) {
			conf >> pint_prob;
			std::cout << "PINT_PROB\t\t\t\t" << pint_prob << '\n';
		}
		fflush(stdout);
	}
	conf.close();

	// overriding config file. I prefer to use cmd arguments
	//cc_mode = algorithm; // overrides configuration file
	//has_win = windowCheck; // overrides configuration file
	//var_win = windowCheck; // overrides configuration file
	std::cout << "----cc_mode " << cc_mode << " has_win " << has_win  << " var_win " << var_win << '\n';

	Config::SetDefault("ns3::QbbNetDevice::PauseTime", UintegerValue(pause_time));
	Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(enable_qcn));
	Config::SetDefault("ns3::QbbNetDevice::QbbEnabled", BooleanValue(enable_pfc));
	if (enable_irn)
	{
		wRdmaGateway::irnEnabled = true;
	}

	if (gatewayXorEnable)
	{
		wRdmaGateway::m_xorEnabled = true;
	}

	if (gatewaySrEnable)
	{
		wRdmaGateway::m_gatewaySrEnabled = true;
	}

	// set int_multi
	IntHop::multi = int_multi;
	// IntHeader::mode
	if (cc_mode == 7) // timely, use ts
		IntHeader::mode = IntHeader::TS;
	else if (cc_mode == 3) // hpcc, powertcp, use int
		IntHeader::mode = IntHeader::NORMAL;
	else if (cc_mode == 10) // hpcc-pint
		IntHeader::mode = IntHeader::PINT;
	else // others, no extra header
		IntHeader::mode = IntHeader::NONE;

	// Set Pint
	if (cc_mode == 10) {
		Pint::set_log_base(pint_log_base);
		IntHeader::pint_bytes = Pint::get_n_bytes();
	}

	topof.open(topology_file.c_str());
	flowf.open(flow_file.c_str());
	uint32_t node_num, switch_num, tors, link_num, trace_num;
	topof >> node_num >> switch_num >> tors >> link_num; // changed here. The previous order was node, switch, link // tors is not used. switch_num=tors for now.
	tors = switch_num;
	std::cout << node_num << " " << switch_num << " " << tors <<  " " << link_num << std::endl;
	flowf >> flow_num;

	NodeContainer serverNodes;
	NodeContainer torNodes;
	NodeContainer spineNodes;
	NodeContainer allNodes;

	std::vector<uint32_t> node_type(node_num, 0);
	std::cout << "switch_num " << switch_num << std::endl;
	for (uint32_t i = 0; i < switch_num; i++) {
		uint32_t sid;
		topof >> sid;
		std::cout << "sid " << sid << std::endl;
		switchNumToId[i] = sid;
		switchIdToNum[sid] = i;
		if (i < tors) {
			node_type[sid] = 1;
		}
		else
			node_type[sid] = 2;

	}

	for (uint32_t i = 0; i < node_num; i++) {
		if (node_type[i] == 0) {
			Ptr<Node> node = CreateObject<Node>();
			n.Add(node);
			allNodes.Add(node);
			serverNodes.Add(node);
		}
		else {
			Ptr<SwitchNode> sw = CreateObject<SwitchNode>();
			n.Add(sw);
			switchNodes.Add(sw);
			allNodes.Add(sw);
			sw->SetAttribute("EcnEnabled", BooleanValue(enable_qcn));
			if (node_type[i] == 1) {
				torNodes.Add(sw);
				sw->SetNodeType(1);
			}
			else {
				spineNodes.Add(sw);
				sw->SetNodeType(2);
			}

		}
	}


	NS_LOG_INFO("Create nodes.");

	InternetStackHelper internet;
	Ipv4GlobalRoutingHelper globalRoutingHelper;
	internet.SetRoutingHelper (globalRoutingHelper);
	internet.Install(n);

	//
	// Assign IP to each server
	//
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() == 0) { // is server
			serverAddress.resize(i + 1);
			serverAddress[i] = node_id_to_ip(i);
		}
	}

	NS_LOG_INFO("Create channels.");

	//
	// Explicitly create the channels required by the topology.
	//

	Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
	Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
	rem->SetRandomVariable(uv);
	uv->SetStream(50);
	rem->SetAttribute("ErrorRate", DoubleValue(error_rate_per_link));
	rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

	FILE *pfc_file = fopen(pfc_output_file.c_str(), "w");

	QbbHelper qbb;
	Ipv4AddressHelper ipv4;
	for (uint32_t i = 0; i < link_num; i++)
	{
		uint32_t src, dst;
		std::string data_rate, link_delay;
		double error_rate;
		topof >> src >> dst >> data_rate >> link_delay >> error_rate;

		std::cout << src << " " << dst << " " << n.GetN() << " " << data_rate << " " << link_delay << " " << error_rate << std::endl;
		Ptr<Node> snode = n.Get(src), dnode = n.Get(dst);

		if (gatewayNodeType.find(src) != gatewayNodeType.end() && gatewayNodeType.find(dst) != gatewayNodeType.end())
		{
			longHaulBandwidth = DataRate(data_rate).GetBitRate();
			longHaulDelay = Time(link_delay);
		}

		qbb.SetDeviceAttribute("DataRate", StringValue(data_rate));
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

		fflush(stdout);

		// Assigne server IP
		// Note: this should be before the automatic assignment below (ipv4.Assign(d)),
		// because we want our IP to be the primary IP (first in the IP address list),
		// so that the global routing is based on our IP
		NetDeviceContainer d = qbb.Install(snode, dnode);
		if (snode->GetNodeType() == 0) {
			Ptr<Ipv4> ipv4 = snode->GetObject<Ipv4>();
			ipv4->AddInterface(d.Get(0));
			ipv4->AddAddress(1, Ipv4InterfaceAddress(serverAddress[src], Ipv4Mask(0xff000000)));
			Ptr<QbbNetDevice> dev =DynamicCast<QbbNetDevice>(d.Get(0));
			if ((bifrostFlag) && (dev != NULL))
			{
				dev->bifrost(bifrostTime,bifrostK,false);
			}
		}
		if (dnode->GetNodeType() == 0) {
			Ptr<Ipv4> ipv4 = dnode->GetObject<Ipv4>();
			ipv4->AddInterface(d.Get(1));
			ipv4->AddAddress(1, Ipv4InterfaceAddress(serverAddress[dst], Ipv4Mask(0xff000000)));
			Ptr<QbbNetDevice> dev =DynamicCast<QbbNetDevice>(d.Get(0));
			if ((bifrostFlag) && (dev != NULL))
			{
				dev->bifrost(bifrostTime,bifrostK,false);
			}
		}


		if (!snode->GetNodeType()) {
			sourceNodes[src].Add(DynamicCast<QbbNetDevice>(d.Get(0)));
		}

		if (!snode->GetNodeType() && dnode->GetNodeType()) {			
			switchDown[switchIdToNum[dst]].Add(DynamicCast<QbbNetDevice>(d.Get(1)));
		}



		if (snode->GetNodeType() && dnode->GetNodeType()) {
			switchToSwitchInterfaces.Add(d);
			switchUp[switchIdToNum[src]].Add(DynamicCast<QbbNetDevice>(d.Get(0)));
			switchUp[switchIdToNum[dst]].Add(DynamicCast<QbbNetDevice>(d.Get(1)));
			switchToSwitch[src][dst].push_back(DynamicCast<QbbNetDevice>(d.Get(0)));
			switchToSwitch[src][dst].push_back(DynamicCast<QbbNetDevice>(d.Get(1)));
		}

		if ((bifrostLinks.find(src) != bifrostLinks.end()) && (bifrostLinks[src] == dst))
		{
			bifrostDevice[DynamicCast<QbbNetDevice>(d.Get(1))] = true;
		}

		if ((bifrostLinks.find(dst) != bifrostLinks.end()) && (bifrostLinks[dst] == src))
		{
			bifrostDevice[DynamicCast<QbbNetDevice>(d.Get(0))] = true;
		}

		gatewayLongHaulDev[src] = DynamicCast<QbbNetDevice>(d.Get(0))->GetIfIndex();
		gatewayLongHaulDev[dst] = DynamicCast<QbbNetDevice>(d.Get(1))->GetIfIndex();

		// used to create a graph of the topology
		nbr2if[snode][dnode].idx = DynamicCast<QbbNetDevice>(d.Get(0))->GetIfIndex();
		nbr2if[snode][dnode].up = true;
		nbr2if[snode][dnode].delay = DynamicCast<QbbChannel>(DynamicCast<QbbNetDevice>(d.Get(0))->GetChannel())->GetDelay().GetTimeStep();
		nbr2if[snode][dnode].bw = DynamicCast<QbbNetDevice>(d.Get(0))->GetDataRate().GetBitRate();
		nbr2if[dnode][snode].idx = DynamicCast<QbbNetDevice>(d.Get(1))->GetIfIndex();
		nbr2if[dnode][snode].up = true;
		nbr2if[dnode][snode].delay = DynamicCast<QbbChannel>(DynamicCast<QbbNetDevice>(d.Get(1))->GetChannel())->GetDelay().GetTimeStep();
		nbr2if[dnode][snode].bw = DynamicCast<QbbNetDevice>(d.Get(1))->GetDataRate().GetBitRate();

		// This is just to set up the connectivity between nodes. The IP addresses are useless
		// char ipstring[16];
		std::stringstream ipstring;
		ipstring << "10." << i / 254 + 1 << "." << i % 254 + 1 << ".0";
		// sprintf(ipstring, "10.%d.%d.0", i / 254 + 1, i % 254 + 1);
		ipv4.SetBase(ipstring.str().c_str(), "255.255.255.0");
		ipv4.Assign(d);

		// setup PFC trace
		DynamicCast<QbbNetDevice>(d.Get(0))->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback (&get_pfc, pfc_file, DynamicCast<QbbNetDevice>(d.Get(0))));
		DynamicCast<QbbNetDevice>(d.Get(1))->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback (&get_pfc, pfc_file, DynamicCast<QbbNetDevice>(d.Get(1))));
	}

	nic_rate = get_nic_rate(n);
	// config switch
	// The switch mmu runs Dynamic Thresholds (DT) by default.
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType()) { // is switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));
			uint32_t shift = 3; // by default 1/8
			double alpha =  1.0 / 8;
			sw->m_mmu->SetAlphaIngress(alpha);
			sw->m_mmu->SetAlphaEgress(UINT16_MAX);
			if (gatewayFlag != 0)
			{
				//sw->m_wRdmaGateway.m_flowPool.wGwFlowPoolTest();
				//sw->m_wRdmaGateway.m_mirrorPool.wGwMirrorPoolTest();
				if (gatewayNodeType.find(i) != gatewayNodeType.end())
				{
					if (gatewayNodeType[i] == 1)
					{
						if (gatewayFlag == 1)
						{
							sw->m_wRdmaGateway.init(gatewayNodeType[i],gatewaySrcPoolSize*1000000,packet_payload_size,gatewayWin,gatewayL2Timeout,gatewayLongHaulDev[i]);
						}
						else if (gatewayFlag == 2)
						{
							sw->m_atcGateway.init(gatewayNodeType[i],max_voq_count,max_voq_rate, longHaulBandwidth,longHaulDelay);
						}
						else if (gatewayFlag == 3)
						{
							// Sentry节点（发送端网关）
							sw->m_lr2Gateway.init(1, 0, 0, 0,packet_payload_size);  // Sentry不需要buffer
						}

						if (enable_qcn)
						{
							NS_ASSERT_MSG(rate2kmin.find(gatewayPeerOutputRate) != rate2kmin.end(), "must set kmin for each link speed");
							NS_ASSERT_MSG(rate2kmax.find(gatewayPeerOutputRate) != rate2kmax.end(), "must set kmax for each link speed");
							NS_ASSERT_MSG(rate2pmax.find(gatewayPeerOutputRate) != rate2pmax.end(), "must set pmax for each link speed");
							sw->m_mmu->ConfigPeerEcn(gatewayPeerOutputRate,rate2kmin[gatewayPeerOutputRate], rate2kmax[gatewayPeerOutputRate], rate2pmax[gatewayPeerOutputRate]); 
						}
					}
					else if (gatewayNodeType[i] == 2)
					{
						if (gatewayFlag == 1)
						{
							sw->m_wRdmaGateway.init(gatewayNodeType[i],gatewayDstPoolSize*1000000,packet_payload_size,gatewayWin,gatewayL2Timeout,gatewayLongHaulDev[i]);

							if (enable_qcn)
							{
								sw->m_wRdmaGateway.ConfigEcn(rate2kmax, rate2kmin, rate2pmax); 
							}
						}
						else if (gatewayFlag == 2)
						{
							sw->m_atcGateway.init(gatewayNodeType[i],max_voq_count,max_voq_rate, longHaulBandwidth,longHaulDelay);
						}
						else if (gatewayFlag == 3)
						{
							// Depot节点（接收端网关）
							sw->m_lr2Gateway.init(2, lr2DepotReorderingSize, lr2DepotBackupSize, lr2DepotBackupTimeout,packet_payload_size);
						}
					}
					else if (gatewayNodeType[i] == 3)
					{
						if (gatewayFlag == 1)
						{
							sw->m_wRdmaGateway.init(1,gatewaySrcPoolSize*1000000,packet_payload_size,gatewayWin,gatewayL2Timeout,gatewayLongHaulDev[i]);
							sw->m_wRdmaGateway.init(2,gatewayDstPoolSize*1000000,packet_payload_size,gatewayWin,gatewayL2Timeout,gatewayLongHaulDev[i]);

							if (enable_qcn)
							{
								NS_ASSERT_MSG(rate2kmin.find(gatewayPeerOutputRate) != rate2kmin.end(), "must set kmin for each link speed");
								NS_ASSERT_MSG(rate2kmax.find(gatewayPeerOutputRate) != rate2kmax.end(), "must set kmax for each link speed");
								NS_ASSERT_MSG(rate2pmax.find(gatewayPeerOutputRate) != rate2pmax.end(), "must set pmax for each link speed");
								sw->m_mmu->ConfigPeerEcn(gatewayPeerOutputRate,rate2kmin[gatewayPeerOutputRate], rate2kmax[gatewayPeerOutputRate], rate2pmax[gatewayPeerOutputRate]); 
								sw->m_wRdmaGateway.ConfigEcn(rate2kmax, rate2kmin, rate2pmax); 
							}
						}
						else if (gatewayFlag == 2)
						{
							sw->m_atcGateway.init(1,max_voq_count,max_voq_rate, longHaulBandwidth,longHaulDelay);
							sw->m_atcGateway.init(2,max_voq_count,max_voq_rate, longHaulBandwidth,longHaulDelay);
						}
						else if (gatewayFlag == 3)
						{
							// 双端网关节点（Sentry + Depot）
							sw->m_lr2Gateway.init(1, 0, 0, 0,packet_payload_size);  // Sentry部分
							sw->m_lr2Gateway.init(2, lr2DepotReorderingSize, lr2DepotBackupSize, lr2DepotBackupTimeout,packet_payload_size);  // Depot部分
						}
					}
				}

			}
			uint64_t totalHeadroom = 0;
			bool receiver = false;
			for (uint32_t j = 1; j < sw->GetNDevices(); j++) {
				Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(sw->GetDevice(j));
				for (uint32_t qu = 0; qu < 8; qu++) {
					// set ecn
					uint64_t rate = dev->GetDataRate().GetBitRate();
					if (enable_qcn)
					{
						NS_ASSERT_MSG(rate2kmin.find(rate) != rate2kmin.end(), "must set kmin for each link speed");
						NS_ASSERT_MSG(rate2kmax.find(rate) != rate2kmax.end(), "must set kmax for each link speed");
						NS_ASSERT_MSG(rate2pmax.find(rate) != rate2pmax.end(), "must set pmax for each link speed");
						sw->m_mmu->ConfigEcn(j, rate2kmin[rate], rate2kmax[rate], rate2pmax[rate]); 
					}
					// set pfc
					uint64_t delay = DynamicCast<QbbChannel>(dev->GetChannel())->GetDelay().GetTimeStep();
					uint32_t headroom = rate * delay / 8 / 1000000000 * 3;

					sw->m_mmu->SetHeadroom(headroom, j, qu);
					totalHeadroom += headroom;
				}

				if (bifrostFlag)
				{
					if (bifrostDevice.find(dev) != bifrostDevice.end()) {
						dev->bifrost(bifrostTime,bifrostK,true); //接收端
						receiver = true;
					}
					else{
						dev->bifrost(bifrostTime,bifrostK,false);
					}
				}

			}

			if (gatewayNodeType.find(i) != gatewayNodeType.end())
			{
				sw->m_mmu->SetBufferPool((uint64_t)gatewaySrcPoolSize * 1024 * 1024 + totalHeadroom);
				sw->m_mmu->SetIngressPool((uint64_t)gatewaySrcPoolSize * 1024 * 1024);
				sw->m_mmu->SetEgressLosslessPool((uint64_t)gatewaySrcPoolSize * 1024 * 1024);
			}
			else
			{
				sw->m_mmu->SetBufferPool(buffer_size * 1024 * 1024 + totalHeadroom);
				sw->m_mmu->SetIngressPool(buffer_size * 1024 * 1024);
				sw->m_mmu->SetEgressLosslessPool(buffer_size * 1024 * 1024);
			}
			sw->m_mmu->node_id = sw->GetId();
		}
	}

#if ENABLE_QP
	FILE *fct_output = fopen(fct_output_file.c_str(), "w");
	//
	// install RDMA driver
	//
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() == 0) { // is server
			// create RdmaHw
			Ptr<RdmaHw> rdmaHw = CreateObject<RdmaHw>();
			rdmaHw->SetAttribute("ClampTargetRate", BooleanValue(clamp_target_rate));
			rdmaHw->SetAttribute("ByteCounterEnabled", BooleanValue(byteCounterEnabled));
			rdmaHw->SetAttribute("ByteCounterThreshold", UintegerValue(byteCounterThreshold));
			rdmaHw->SetAttribute("CnpGenerationInterval", UintegerValue(cnpGenerationInterval));
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
			rdmaHw->SetAttribute("PowerTCPEnabled", BooleanValue(wien));
			rdmaHw->SetAttribute("PowerTCPdelay", BooleanValue(delayWien));
			rdmaHw->SetAttribute("L2Timeout", TimeValue(MilliSeconds(l2timeout))); 
			if (enable_irn)
			{
				rdmaHw->SetAttribute("IrnEnable", BooleanValue(enable_irn));
				rdmaHw->SetAttribute("IrnRtoLow", TimeValue(Time(irn_rto_min)));
				rdmaHw->SetAttribute("IrnRtoHigh", TimeValue(Time(irn_rto_max)));
				rdmaHw->SetAttribute("IrnBdp", UintegerValue(irn_bdp)); // in bytes
			}

			rdmaHw->SetPintSmplThresh(pint_prob);
			// create and install RdmaDriver
			Ptr<RdmaDriver> rdma = CreateObject<RdmaDriver>();
			Ptr<Node> node = n.Get(i);
			rdma->SetNode(node);
			rdma->SetRdmaHw(rdmaHw);

			node->AggregateObject (rdma);
			rdma->Init();
			rdma->TraceConnectWithoutContext("QpComplete", MakeBoundCallback (qp_finish, fct_output));
		}
	}

#endif

	// set ACK priority on hosts
	if (ack_high_prio)
		RdmaEgressQueue::ack_q_idx = 0;
	else
		RdmaEgressQueue::ack_q_idx = 3;

	// setup routing
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
			sw->SetAttribute("PowerEnabled", BooleanValue(wien));
		}
	}

	Ipv4GlobalRoutingHelper::PopulateRoutingTables();
	// Ptr<OutputStreamWrapper> routingStream = Create<OutputStreamWrapper> (&std::cout);
	// globalRoutingHelper.PrintRoutingTableAt (Seconds (0.0), n.Get(0), routingStream);

	NS_LOG_INFO("Create Applications.");

	Time interPacketInterval = Seconds(0.0000005 / 2);


	// maintain port number for each host
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() == 0)
			for (uint32_t j = 0; j < node_num; j++) {
				if (n.Get(j)->GetNodeType() == 0)
					portNumder[i][j] = 10000; // each host pair use port number from 10000
			}
	}

	flow_input.idx = 0;
	if (flow_input.flows_rand_inner > 0 || flow_input.flows_rand_inter > 0)
	{
		flow_input.pg=3;
		flow_input.maxPacketCount = flow_input.flows_rand_packets;
		flow_input.start_time = flow_input.flows_rand_start_time;
		flow_num = flow_input.flows_rand_inner + flow_input.flows_rand_inter;

		if (flow_input.flows_rand_inner > 0 )
		{
			Simulator::Schedule(Seconds(flow_input.start_time) + MicroSeconds(maxRtt/2000) - Simulator::Now(), ScheduleInnerFlow);
		}

		if (flow_input.flows_rand_inter > 0)
		{
			Simulator::Schedule(Seconds(flow_input.start_time) - Simulator::Now(), ScheduleInterFlow);
		}

	}
	else
	{
		
		if (flow_num > 0) {
			ReadFlowInput();
			std::cout << flow_input.start_time << std::endl;
			Simulator::Schedule(Seconds(flow_input.start_time) - Simulator::Now(), ScheduleFlowInputs);
		}
	}

	topof.close();
	tracef.close();
	double delay = (double)LogIntervalUs * 1e-6;  // 10 micro seconds
	Simulator::Schedule(Seconds(delay), PrintResults, switchNodes, switch_num, delay);

	if (enable_trace != 0)
	{
		AsciiTraceHelper ascii;
			qbb.EnableAsciiAll (ascii.CreateFileStream ("eval.tr"));
		//qbb.EnablePcapAll( "eval.pcap");
	}
	std::cout << "Running Simulation.\n";
	NS_LOG_INFO("Run Simulation.");
	Simulator::Stop(Seconds(simulator_stop_time));
	Simulator::Run();
	Simulator::Destroy();
	NS_LOG_INFO("Done.");
	endt = clock();
	std::cout << (double)(endt - begint) / CLOCKS_PER_SEC << "\n";
}
