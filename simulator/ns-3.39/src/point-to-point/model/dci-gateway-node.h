#ifndef DCI_GATEWAY_NODE_H
#define DCI_GATEWAY_NODE_H
#include "switch-node.h"
#include <array>
#include <fstream>
#include <map>
#include <tuple>
#include <vector>

namespace ns3 {
// Fixed registration: the IP/UDP/PG wire identity cannot be reused within a run.
class DciGatewayNode : public SwitchNode {
public:
    static TypeId GetTypeId();
    enum class Role { Source, Receiver };
    enum class Life { NotStarted, Active, Idle, Draining, Closed };
    enum class ServiceSource { Paused, BusyObserved, BudgetFallback, BudgetOnly };
    enum class TxMode { Normal, StartupProbe, RecoveryProbe, Stopped, OptimisticStartup };
    struct Config {
        bool reconstruct=true, shaperEcn=true, nearCnp=true;
        std::string ecnMode="source-boundary", startupMode="optimistic";
        std::string serviceMode="busy-observed";
        double period=.0002, control=.00005, bin=.00005;
        double reaction=.00005, gamma=.0625, recovery=.0002;
        double probeRate=125000000, increase=625000000, utilization=.98;
        double qref=500000, tau=.02, timeout=.02, fallback=.05;
        double sourceHigh=250000, sourceLow=62500, cnpInterval=.00005, reportMin=.00005;
        double serviceWindow=.0002, serviceMinBusy=.00005, probePortFraction=.05;
        uint64_t probeBytes=65536, probePortBurst=65536, bufferBytes=50*1024*1024;
        uint32_t controlIpMtu=1500;
    };
    struct Registration {
        uint32_t id=0, generation=1, peer=0, port=0, pg=3, frameBytes=1048, sip=0, dip=0;
        uint16_t sport=0, dport=0;
        Role role=Role::Source;
        Time forwardDelay, feedbackRtt, start;
        double peerPortBytesPerSec=0;
    };
    void Configure(const Config&,const std::string& output);
    void RegisterFlow(const Registration&);
    void Start();
    void CompleteFlow(uint32_t id);
    void WriteMetadata(std::ostream&) const;
    bool SwitchReceiveFromDevice(Ptr<NetDevice>,Ptr<Packet>,CustomHeader&) override;
    static Ipv4Address ControlAddress(uint32_t node);
protected:
    void DoDispose() override;
    bool ShouldMarkEcn(uint32_t,Ptr<const Packet>) override;
private:
    friend class DciGatewayTestCase;
    enum : uint64_t { HAS_DEMAND=1, BACKLOG=2, NO_MORE_HOST_DATA=4, SENDABLE=8 };
    enum : uint32_t { STATE=1, DEMAND=2 };
    using Record=std::array<uint64_t,19>;
    struct History {
        int64_t width=1, latest=0;
        std::vector<uint64_t> bins;
        void Init(double widthSeconds,double retention);
        void Advance(int64_t now);
        void Add(int64_t now,uint32_t bytes);
        bool Covers(int64_t begin,int64_t end) const;
        double Predict(int64_t sample,int64_t delay,int64_t now,double queue,double service) const;
    };
    struct ServiceSlice {
        int64_t begin=0,end=0,busy=0,pause=0;
        uint64_t bytes=0;
    };
    struct ServiceWindow {
        std::vector<ServiceSlice> slices;
        size_t next=0,count=0;
        int64_t last=0,begin=0,busy=0,previousPause=0;
        uint64_t bytes=0;
        bool backlogged=false;
        ServiceSlice observation;
        void Init(size_t length,int64_t now,int64_t pause);
        void AccountUntil(int64_t now);
        void Close(int64_t now,int64_t pause);
    };
    struct SourceState {
        History history;
        Record state{};
        bool have=false,fresh=false,covered=false,startupGranted=false,recoveryGranted=false;
        bool activeStateReceived=false;
        uint64_t probeEpoch=0,lastGrantRx=0,grantSeq=0,grantSentBaseline=0;
        bool pendingGrant=false;
        double predicted=0,used=0,candidate=0;
        TxMode mode=TxMode::Stopped;
        const char* reason="not_started";
    };
    struct ReceiverState {
        bool started=false;
        Record demand{};
        ServiceWindow window;
        double service=0;
        uint64_t queueTarget=0;
        int64_t lastBudgetChange=0;
        ServiceSource serviceSource=ServiceSource::BudgetFallback;
    };
    struct Flow {
        Registration reg;
        uint32_t queue=0;
        Life life=Life::NotStarted;
        uint64_t in=0,tx=0,peak=0,previousTx=0,cnps=0,decreases=0,sequence=0;
        int64_t lastArrival=-1,lastCnp=-1,lastNearCnp=-1,reactionStart=0,recoverySince=0,lastReport=-1;
        double rate=0,budget=0,target=0,alpha=0,sourceHigh=0,sourceLow=0;
        bool event=false,active=false,closed=false,probing=true,paused=false,dirty=true,marking=false,nearPending=false;
        bool upstreamCePending=false;
        uint64_t upstreamCe=0,nearCnps=0;
        SourceState source;
        ReceiverState receiver;
        uint64_t Queue() const { return in-tx; }
        bool IsSource() const { return reg.role==Role::Source; }
    };
    struct PortState {
        std::vector<uint32_t> flows;
        std::map<uint32_t,uint32_t> queueToFlow;
        uint64_t otherTx=0,previousOtherTx=0;
        int64_t lastSample=-1;
        double physical=0,available=0;
    };
    using DataKey=std::tuple<uint32_t,uint32_t,uint16_t,uint16_t,uint16_t>;
    using FeedbackKey=std::tuple<uint32_t,uint32_t,uint16_t,uint16_t>;
    Config m_config;
    bool m_enabled=false,m_started=false,m_sourceOverload=false,m_receiverOverload=false;
    std::map<uint32_t,Flow> m_flows;
    std::map<uint32_t,PortState> m_ports;
    std::map<DataKey,uint32_t> m_data;
    std::map<FeedbackKey,uint32_t> m_feedback;
    std::ofstream m_log,m_portLog,m_events,m_packets;
    EventId m_tick;
    uint64_t m_controlTx=0,m_controlRx=0,m_rejected=0,m_unmatched=0,m_ordinal=0;
    std::vector<Ptr<Packet>> m_reports;
    static std::vector<double> Allocate(const std::vector<double>&,double capacity);
    static uint32_t Classify(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    static void Admitted(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t,uint32_t);
    static void Dequeue(DciGatewayNode*,uint32_t,Ptr<const Packet>,uint32_t);
    Flow* Match(Ptr<const Packet>);
    Ptr<QbbNetDevice> Port(uint32_t) const;
    void ObserveFeedback(CustomHeader&);
    void ConsumeSourceCe(Ptr<Packet>,CustomHeader&);
    void SamplePorts();
    void UpdateDemand();
    void UpdateReceiver();
    void UpdateSource();
    void PrepareProbeGrant(Flow&,bool startup);
    void BuildReports();
    void ReceiveRecords(Ptr<Packet>,const CustomHeader&);
    bool ApplyRecord(Flow&,const Record&,uint32_t kind,const char*& reason);
    void CommitControls();
    void WriteSamples();
    void Tick();
    void SendNearCnp(Flow&);
    void Event(const char*,uint32_t,double,const char* reason="");
    void PacketEvent(Flow&,const char*,uint32_t);
};
}
#endif
