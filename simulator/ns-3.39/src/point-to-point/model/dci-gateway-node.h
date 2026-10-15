#ifndef DCI_GATEWAY_NODE_H
#define DCI_GATEWAY_NODE_H
#include "switch-node.h"
#include <array>
#include <deque>
#include <fstream>
#include <map>
#include <tuple>

namespace ns3 {
// R5: connection/path associations contain no demand, flow size or completion state.
class DciGatewayNode : public SwitchNode {
public:
    static TypeId GetTypeId();
    enum class Role { Source, Receiver };
    struct Config {
        double period=.0002, control=.00005, tau=.002, timeout=.02;
        double coldFraction=.01, conservativeFraction=.05;
        double qref=50000, margin=65536;
        double sourceHigh=1000000, sourceLow=250000, localWait=.0003, cnpInterval=.00005;
        double localWindow=.0002;
        uint64_t bufferBytes=50*1024*1024;
    };
    struct Registration {
        uint32_t id=0, peer=0, port=0, remotePort=0, pg=3, frameBytes=1048, sip=0, dip=0;
        uint16_t sport=0, dport=0;
        Role role=Role::Source;
        // Propagation + receiver processing only; per-packet serialization is added at actual TX.
        Time propagation, localResponse;
    };
    void Configure(const Config&,const std::string& output);
    void RegisterFlow(const Registration&);
    void Start();
    void WriteMetadata(std::ostream&) const;
    bool SwitchReceiveFromDevice(Ptr<NetDevice>,Ptr<Packet>,CustomHeader&) override;
    static Ipv4Address ControlAddress(uint32_t node);
protected:
    void DoDispose() override;
    void NotifyAdmissionDrop(uint32_t,uint32_t,Ptr<const Packet>) override;
private:
    friend class DciGatewayTestCase;
    enum Field { GROUP, SEQ, SAMPLE, QUEUE, ARRIVED, DEPARTED, DROPPED, OTHER,
                 SERVICE, BACKGROUND, CAPACITY, BUFFER, FLAGS, EMPTY, PAUSE_NS, ORDINAL, CONTROLLED, FIELDS };
    enum : uint64_t { VALID=1, PAUSED=2, SHARED=4 };
    using Record=std::array<uint64_t,FIELDS>;
    struct Prediction { double queue=0, peak=0; };
    struct History {
        struct Arrival { int64_t time; uint32_t bytes; uint64_t cumulative; };
        std::deque<Arrival> arrivals;
        int64_t floor=0;
        uint64_t total=0;
        void Add(int64_t arrival,uint32_t bytes);
        void Prune(int64_t before);
        bool Covers(int64_t sample) const { return sample>=floor; }
        Prediction Replay(int64_t sample,int64_t end,int64_t peakBegin,double queue,double net,uint64_t sampledControlled=UINT64_MAX) const;
    };
    struct Flow {
        Registration reg;
        uint32_t group=0, queue=0;
        uint64_t in=0, tx=0, previousIn=0, previousTx=0, nearCnps=0, nativeCnps=0;
        int64_t lastCnp=-1, lastNativeCnp=-1;
    };
    struct Group {
        uint32_t id=0, peer=0, port=0, pg=0, frame=0;
        Role role=Role::Source;
        Time propagation, localResponse;
        std::vector<uint32_t> flows;
        uint64_t in=0, tx=0, drops=0, other=0, controlled=0, previousTx=0, previousOther=0;
        uint64_t previousLocalIn=0, previousLocalTx=0;
        uint64_t seq=0, peak=0;
        int64_t lastSample=0, previousPause=0, serviceInterval=0;
        bool emptied=true, shared=false, serviceValid=false, protecting=false;
        double service=0, background=0, capacity=0, buffer=0, pressure=0, growth=0;
        double availability=1;
        // candidate is a controller cap; target is only the current port allocation.
        double target=0, candidate=0;
        EventId serviceReport;
        const char* reason="cold";
        History history;
        Record state{};
        bool have=false, fresh=false, covered=false, peakLimited=false;
        uint64_t peakSeq=0;
        unsigned badResiduals=0;
        std::map<int64_t,double> forecasts;
        Prediction predicted;
        int64_t predictionEnd=0;
        uint64_t Queue() const { return in-tx; }
        bool IsSource() const { return role==Role::Source; }
    };
    using DataKey=std::tuple<uint32_t,uint32_t,uint16_t,uint16_t,uint16_t>;
    Config m_config;
    bool m_enabled=false,m_started=false;
    std::map<uint32_t,Flow> m_flows;
    std::map<uint32_t,Group> m_groups;
    std::map<DataKey,uint32_t> m_data;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> m_receiverGroups;
    std::map<uint32_t,std::vector<uint32_t>> m_sourcePorts;
    std::ofstream m_log,m_events,m_packets,m_groupPackets;
    EventId m_tick,m_activation;
    uint64_t m_ordinal=0,m_controlTx=0,m_controlRx=0,m_rejected=0;
    static uint32_t Classify(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    static void Admitted(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t,uint32_t);
    static void Dequeue(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    static void PauseChanged(DciGatewayNode*,uint32_t,uint32_t,bool);
    Flow* Match(Ptr<const Packet>);
    Ptr<QbbNetDevice> Port(uint32_t) const;
    double BufferAllowance(const Group&) const;
    void SampleReceiver(Group&);
    void ReportServiceChange(uint32_t group);
    void UpdateSource(Group&);
    void UpdateBackpressure(Group&);
    bool NeedCnp(Group&,Flow&,bool emergency,bool trigger,bool draining,int64_t now);
    void CommitControls();
    void ActivateSources();
    void SendReport(Group&);
    void ReceiveReport(Ptr<Packet>,const CustomHeader&);
    bool ApplyRecord(Group&,const Record&);
    void SendNearCnp(Flow&,const char* reason);
    void Event(const char*,uint32_t,double,const char* reason="");
    void PacketEvent(Flow&,const char*,uint32_t);
    void GroupEvent(Group&,const char*,uint32_t);
    void WriteSamples();
    void Tick();
};
}
#endif
