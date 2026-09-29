#ifndef DCI_GATEWAY_NODE_H
#define DCI_GATEWAY_NODE_H

#include "switch-node.h"
#include <array>
#include <fstream>
#include <map>
#include <tuple>
#include <vector>

namespace ns3 {

// R3 uses the simulator's registered UDP QP identity, not hardware QPNs.
// Mappings are fixed for a run; ambiguous/reused wire identities are rejected.
class DciGatewayNode : public SwitchNode {
public:
    static TypeId GetTypeId();
    struct Config {
        bool reconstruct=true;
        bool shaperEcn=true;
        double period=0.0002, control=0.00005, bin=0.00005;
        double reaction=0.00005, gamma=0.0625, recovery=0.0002;
        double probeRate=125000000, increase=625000000, utilization=0.98;
        double qref=500000, tau=0.02, timeout=0.02, fallback=0.05;
        double sourceHigh=250000, sourceLow=62500, cnpInterval=0.00005;
        double reportMin=0.00005;
        uint32_t probeBytes=65536;
        uint64_t bufferBytes=50*1024*1024;
    };
    struct Registration {
        uint32_t id=0, generation=1, group=0, peer=0, port=0, pg=3, burst=1048;
        uint32_t sip=0, dip=0;
        uint16_t sport=0, dport=0;
        bool source=false;
        double delay=0, feedbackRtt=0, start=0;
    };
    void Configure(const Config& config, const std::string& output);
    void RegisterFlow(const Registration& registration);
    void Start();
    void CompleteFlow(uint32_t id);
    void WriteMetadata(std::ostream& out) const;
    bool SwitchReceiveFromDevice(Ptr<NetDevice>, Ptr<Packet>, CustomHeader&) override;
    static Ipv4Address ControlAddress(uint32_t node);

protected:
    void DoDispose() override;
    bool ShouldMarkEcn(uint32_t port, Ptr<const Packet> packet) override;

private:
    friend class DciGatewayTestCase;
    // One complete snapshot, fragmented on the wire and applied atomically.
    struct Record { uint64_t id=0, generation=0, rate=0, flags=0; };
    struct Snapshot {
        uint64_t kind=0, group=0, epoch=1, seq=0, sample=0, queue=0, service=0, budget=0;
        uint64_t flags=0;
        std::vector<Record> records;
    };
    struct Assembly {
        Snapshot value;
        uint32_t parts=0;
        std::map<uint32_t,std::vector<Record>> fragments;
    };
    struct History {
        int64_t width=1, latest=0;
        std::vector<uint64_t> bins;
        void Init(double widthSeconds,double retention);
        void Advance(int64_t now);
        void Add(int64_t now,uint32_t bytes);
        bool Covers(int64_t begin,int64_t end) const;
        double Predict(int64_t sample,int64_t delay,int64_t now,double queue,double service) const;
    };
    struct Flow {
        Registration reg;
        uint32_t queue=0;
        uint64_t in=0, tx=0, previousTx=0, cnps=0, decreases=0;
        double rate=0, budget=0, target=0, alpha=0;
        double lastSeen=-1, lastCnp=-1, lastNearCnp=-1, window=0, recoverySince=0;
        bool event=false, demand=false, closed=false, active=false, probing=true;
    };
    struct Group {
        uint32_t id=0, peer=0, port=0, pg=0;
        bool source=false, marking=false, have=false, dirty=false;
        double delay=0, feedbackRtt=0, lastSend=-1, demandTime=-1;
        double service=0, budget=0, target=0, predicted=0;
        double queueUsed=0, unconstrained=0;
        bool fresh=false;
        uint64_t sequence=0, demandSeq=0, peak=0;
        std::vector<uint32_t> flows;
        History history;
        Snapshot snapshot;
        Assembly assembly;
    };
    using DataKey=std::tuple<uint32_t,uint32_t,uint16_t,uint16_t,uint16_t>;
    using FeedbackKey=std::tuple<uint32_t,uint32_t,uint16_t,uint16_t>;
    Config m_config;
    bool m_enabled=false, m_receiverOverload=false;
    std::map<uint32_t,Flow> m_flows;
    std::map<uint32_t,Group> m_groups;
    std::map<DataKey,uint32_t> m_data;
    std::map<FeedbackKey,uint32_t> m_feedback;
    std::map<uint32_t,uint32_t> m_nextQueue;
    std::map<uint32_t,uint64_t> m_otherTx;
    std::ofstream m_log, m_events, m_packets;
    EventId m_tick;
    uint64_t m_controlTx=0, m_controlRx=0, m_rejected=0, m_unmatched=0;
    static std::vector<double> Allocate(const std::vector<double>& caps,double capacity);
    static uint32_t Classify(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    static void Enqueue(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    static void Dequeue(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    Flow* Match(Ptr<const Packet>);
    Ptr<QbbNetDevice> Port(uint32_t) const;
    uint64_t Queue(const Group&) const;
    void ObserveFeedback(CustomHeader&);
    void Tick();
    void UpdateReceiver();
    void UpdateSource();
    void SendSnapshot(Group&);
    void ReceiveSnapshot(Ptr<Packet>,const CustomHeader&);
    void SendNearCnp(Flow&);
    void Event(const char*,uint32_t,double);
};
}
#endif
