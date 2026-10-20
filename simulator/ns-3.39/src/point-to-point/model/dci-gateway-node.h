#ifndef DCI_GATEWAY_NODE_H
#define DCI_GATEWAY_NODE_H
#include "switch-node.h"
#include <array>
#include <deque>
#include <fstream>
#include <map>
#include <tuple>

namespace ns3 {
// B owns opaque resource IDs. PG is a local queue attribute, never flow identity.
class R5Header : public Header {
public:
    enum Type { PROBE=1, PROBE_REPLY, FLOW_STATE,
                RESOURCE_STATE, REGISTER, REGISTER_ACK, REGISTER_CONFIRM,
                COMPLETE, COMPLETE_ACK, RETIRE, RETIRE_ACK, FAST_CNP };
    enum Field { SESSION, GEN, PEER_GEN, QUERY_ID, SEQ, SAMPLE, SEND,
        SIP, DIP, QPN, SRC_QPN, CONNECTION, RESOURCE, PORT_RESOURCE,
        QUEUE, ARRIVED, DEPARTED, DROPPED, OTHER, SERVICE, BACKGROUND,
        CAPACITY, BUFFER, FLAGS, PAUSE_NS, INTERVAL_NS, UNPAUSED,
        BASE_DELAY_NS, LINK_RATE, POOL_QUEUE, ASSOCIATION, FIELDS };
    enum Flag : uint64_t { VALID=1, PAUSED=2, EMPTIED=4, PORT=8, POOL=16, FULL=32, APPROXIMATE=64, FAST_CNP_CAPABLE=128 };
    using Record=std::array<uint64_t,FIELDS>;
    uint32_t type=PROBE;
    Record v{};
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override { return 8+8*FIELDS; }
    void Serialize(Buffer::Iterator) const override;
    uint32_t Deserialize(Buffer::Iterator) override;
    void Print(std::ostream&) const override;
};
class DciGatewayNode : public SwitchNode {
public:
    static TypeId GetTypeId();
    struct FlowKey {
        uint32_t srcIp=0, dstIp=0, destQpn=0;
        bool operator<(const FlowKey& other) const {
            return std::tie(srcIp,dstIp,destQpn)<std::tie(other.srcIp,other.dstIp,other.destQpn);
        }
    };
    struct Config {
        double period=.00001, control=.00001, timeout=.02;
        double conservativeFraction=.05;
        double qref=50000, margin=65536;
        double sourceHigh=1000000, sourceLow=250000, localWait=.0003, cnpInterval=.00005;
        double localWindow=.0002, initialDelay=.005, probePeriod=.001, queryRetry=.02;
        double controlFraction=.01;
        uint32_t controlBurstBytes=65536, controlQueueBytes=65536;
        uint64_t bufferBytes=50*1024*1024;
        uint32_t maxFlows=4096;
        uint32_t maxForecasts=1024, maxFuturePackets=262144, maxReports=64, maxRetries=8;
        uint64_t session=1, peerSession=1;
    };
    void Configure(const Config&,const std::string& output);
    void SetPeer(uint32_t peer,uint32_t wanPort,uint32_t frameBytes);
    // After SetPeer and before Start; Fast CNP applies only to this flow's LAN->WAN source role.
    void SetDestinationBudget(uint32_t destinationIp,double bytesPerSecond);
    void Start();
    void WriteMetadata(std::ostream&) const;
    bool SwitchReceiveFromDevice(Ptr<NetDevice>,Ptr<Packet>,CustomHeader&) override;
    bool PrepareDataAdmission(Ptr<Packet>,CustomHeader&,uint32_t,uint32_t,uint32_t) override;
    bool HandleLocalCongestion(uint32_t,uint32_t,Ptr<Packet>,CustomHeader&) override;
    static Ipv4Address ControlAddress(uint32_t node);
    static Ptr<Packet> ControlPacket(R5Header,Ipv4Address src,Ipv4Address dst);
protected:
    void DoDispose() override;
    void NotifyAdmissionDrop(uint32_t,uint32_t,Ptr<const Packet>) override;
private:
    using Record=R5Header::Record;
    // begin < 0 denotes a modeled future packet; otherwise bytes are uniform
    // over [begin,time). Actual TX history is never expanded into packets.
    struct Arrival { int64_t time; uint64_t bytes; int64_t begin=-1; };
    struct TxSnapshot {
        int64_t timeNs;
        uint64_t cumulativeBytes;
        int64_t delayNs=0; // Frozen mapping for the interval ending here.
    };
    struct ArrivalChange { int64_t time; double rate=0, bytes=0; };
    struct Prediction { double queue=0, peak=0; };
    struct History {
        std::deque<TxSnapshot> txSnapshots;
        int64_t floor=0;
        void Add(int64_t,uint64_t,int64_t);
        size_t Prune(int64_t);
        bool Covers(int64_t t) const { return t>=floor; }
    };
    struct Forecast {
        uint64_t id;
        int64_t decision, sample, end;
        double initial, service, predicted, planned;
    };
    struct Flow {
        FlowKey key;
        uint32_t id=0, port=0, pg=0, queue=0, resource=0, portResource=0;
        uint16_t sport=0,dport=0;
        bool source=false, have=false, completed=false, retireAck=false;
        uint64_t finalWireBytes=0;
        int64_t lastMappedArrival=0, retireSent=-1;
        uint64_t generation=0, remoteGeneration=0, seq=0;
        uint64_t in=0, dequeued=0, tx=0, drops=0, failures=0, previousIn=0, previousTx=0;
        uint64_t supplyIn=0, nearCnps=0, nativeCnps=0;
        int64_t created=0, lastData=0, lastCnp=-1, lastNative=-1, supplySample=0;
        double supply=0, candidate=0, target=0, service=0;
        double responseBaseline=0;
        bool responsePending=false;
        const char* reason="startup";
        History history;
        Record state{};
        std::deque<uint32_t> packets;
        uint64_t Queue() const { return in-dequeued; }
    };
    struct Resource {
        uint32_t id=0, port=0, pg=0;
        uint64_t in=0, out=0, other=0, dropped=0, seq=0, lastOut=0, lastOther=0;
        int64_t sample=0,pause=0;
        double service=0;
        bool valid=false, emptied=true;
        Record state{};
    };
    struct SourceRegistration {
        uint32_t sourceQpn=0;
        uint64_t connection=0, query=0;
        int64_t ackSent=0;
        double response=0, localRtt=0;
        bool confirmed=false, fastCnp=false;
    };
    struct ResourceControl {
        double net=0, budget=0;
        Prediction effective, risk;
        bool covered=true;
        int64_t sample=0;
        const char* reason="resource_budget";
    };
    std::map<uint32_t,ResourceControl> m_resourceControls;
    std::map<uint32_t,int64_t> m_retiredHistoryFloor;
    uint64_t m_reallocations=0, m_retiredCount=0;
    bool m_modelBlocked=false;
    std::string m_retiredPath;
    std::ofstream m_retired;
    struct Allocation { uint32_t id; double cap; std::vector<uint64_t> resources; };
    Config m_config;
    uint32_t m_peer=0,m_wan=0,m_frame=0,m_nextFlow=1,m_nextResource=1,m_reportCursor=0;
    bool m_enabled=false,m_started=false,m_peerFull=false,m_backpressureActive=false;
    uint64_t m_sourceIngressCe=0,m_sourceClearedCe=0;
    uint64_t m_fastCnpCandidates=0,m_fastCnpSubmitted=0,m_fastCnpTx=0,m_fastCnpTxFailed=0;
    uint64_t m_fastCnpLimited=0,m_fastCnpUnconfirmed=0,m_fastCnpSubmitFailed=0,m_fastCnpReplaced=0;
    uint64_t m_fastCnpCoalesced=0;
    uint64_t m_fastCnpIngressHandled=0,m_fastCnpIngressCoalesced=0;
    int64_t m_forwardBase=0,m_reverseBase=0,m_lastBackpressure=0;
    int64_t m_lastPeerPressure=0, m_controlUpdated=0;
    int64_t m_lastReceiverSample=-1;
    uint64_t m_pressureSequence=0,m_controlDropped=0;
    double m_controlTokens=0;
    uint64_t m_probe=0,m_lastProbeReply=0,m_historyCount=0,m_decision=0;
    enum Trigger { ARRIVAL, EMPTY, FLOW_FEEDBACK, RESOURCE_FEEDBACK, PRESSURE, PAUSE, TRIGGERS };
    std::array<uint64_t,TRIGGERS> m_triggers{};
    uint64_t m_ticks=0,m_coalesced=0,m_safetyStops=0;
    uint64_t m_forecastCreated=0,m_forecastValidated=0,m_forecastUnavailable=0,m_forecastExpired=0;
    uint64_t m_forecastRejected=0,m_forecastPeak=0,m_historyPeak=0,m_flowHistoryPeak=0;
    uint64_t m_historyCopies=0,m_historyCopiedRecords=0,m_futureModels=0,m_futurePackets=0;
    bool m_controlPending=false;
    uint64_t m_ordinal=0,m_controlTx=0,m_controlRx=0,m_rejected=0,m_resourceDrops=0;
    std::map<uint32_t,Flow> m_flows;
    std::map<FlowKey,uint32_t> m_data;
    std::map<std::pair<uint32_t,uint32_t>,Resource> m_resources;
    std::map<uint32_t,Record> m_remoteResources;
    std::map<uint32_t,std::deque<Forecast>> m_resourceForecasts;
    std::deque<std::pair<uint32_t,Record>> m_validationSamples;
    std::map<FlowKey,SourceRegistration> m_registrations;
    std::map<uint32_t,double> m_destinationBudgets;
    std::ofstream m_log,m_events,m_packets,m_resourceLog,m_predictions,m_budgetLog;
    EventId m_tick,m_report,m_probeEvent,m_serviceReport,m_reallocate;
    static uint32_t Classify(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    static void Admitted(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t,uint32_t);
    static void Dequeue(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    static void Transmitted(DciGatewayNode*,uint32_t,Ptr<const Packet>,bool);
    static void PauseChanged(DciGatewayNode*,uint32_t,uint32_t,bool);
    void Stamp(Ptr<Packet>);
    Flow* Match(Ptr<const Packet>);
    Ptr<QbbNetDevice> Port(uint32_t) const;
    Resource& QueueResource(uint32_t,uint32_t);
    bool BindQueue(Flow&);
    void ReleaseEmptyQueues();
    bool CanRetire(const Flow&) const;
    void ReclaimFlows();
    void EraseFlow(uint32_t);
    void WriteFlowMetadata(std::ostream&,const Flow&,bool retired=false) const;
    bool Send(R5Header,Ipv4Address destination);
    void ReceiveControl(Ptr<Packet>,const CustomHeader&);
    bool RecordForecast(uint32_t,const Forecast&);
    void ValidateForecasts(uint32_t,const Record&);
    void MaintainHistory(Flow&,int64_t now);
    Record FlowRecord(Flow&);
    void Probe();
    void SampleReceiver();
    void PeriodicReport();
    void RequestControl(Trigger);
    void StopResource(uint32_t);
    void ActivateSources();
    void AllocateSources();
    void ReallocateSources();
    void UpdateBackpressure();
    double LocalCnpWait(const Flow&) const;
    bool SendNearCnp(Flow&,const char*);
    bool HandleFastCnp(Flow&,Ptr<Packet>,CustomHeader&,bool upstreamCe);
    void Tick();
    static Prediction Replay(int64_t sample,int64_t end,int64_t peakBegin,double queue,double service,
                             const std::vector<Arrival>& arrivals);
    static std::vector<ArrivalChange> ArrivalChanges(int64_t,int64_t,const std::vector<Arrival>&);
    static std::map<uint32_t,double> Allocate(const std::vector<Allocation>&,
                                             const std::map<uint64_t,double>& budgets);
    bool FutureDepartures(int64_t end,std::map<uint32_t,std::vector<Arrival>>&);
    std::vector<Arrival> Arrivals(const Flow&);
    void Event(const char*,uint32_t,double,const char* reason="");
    void PacketEvent(Flow&,const char*,uint32_t,int64_t arrival=-1);
};
}
#endif
