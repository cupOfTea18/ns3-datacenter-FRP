#ifndef __ATC_GATEWAY_H__
#define __ATC_GATEWAY_H__

#include <map>
#include <queue>
#include <unordered_map>
#include <vector>
#include <bitset>
#include <unordered_set>

#include "ns3/address.h"
#include "ns3/callback.h"
#include "ns3/custom-header.h"
#include "ns3/event-id.h"
#include "ns3/log.h"
#include "ns3/net-device.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/simulator.h"
#include "qbb-net-device.h"

namespace ns3{

#define PRINT_LOG 1

// ========== VOQ相关数据结构 ==========
const uint32_t VOQ_MAX_SIZE = 10 * 1500;        // 每个VOQ最大大小（10个MTU）
const double N_THRESHOLD_RATIO = 2.0/3.0;       // VOQ过载阈值
const double M = 2.0;                           // 带宽保护因子
const Time LONGHAUL_FEEDBACK_PERIOD = MicroSeconds(10); // 长距离反馈周期
const Time CNP_NOTIFY_INTERVAL = MicroSeconds(4); // CNP通知间隔
const Time INTRA_DC_DELAY = MicroSeconds(6); // 内部DC延迟时间

typedef struct {
    DataRate m_targetRate;	//< Target rate
    EventId m_eventUpdateAlpha;
    double m_alpha;
    bool m_alpha_cnp_arrived; // indicate if CNP arrived in the last slot
    bool m_first_cnp; // indicate if the current CNP is the first CNP
    EventId m_eventDecreaseRate;
    bool m_decrease_cnp_arrived; // indicate if CNP arrived in the last slot
    uint32_t m_rpTimeStage;
    EventId m_rpTimer;
} MLX1;


// VOQ条目结构（增加DCQCN状态）
struct VoqEntry {
    uint32_t voqId;
    Ipv4Address destIp;
    uint64_t currentSize;              // 当前队列大小（字节）
    uint64_t maxSize;                  // 最大队列大小
    bool isAllocated;                  // 分配状态
    std::queue<Ptr<Packet>> packetQueue;    // 数据包队列
    EventId transmitEvent;             // 转发定时器
    EventId voqReleaseEvent;             // VOQ释放定时器
    Time lastTransmitTime;             // 最后转发时间

    uint64_t currentInflightBytes; // 当前在飞行中的字节数（字节）
    uint64_t maxInflightBytes; // 最大飞行字节数（字节）
    
    // ========== DCQCN状态（基于VOQ的拥塞控制） ==========
    MLX1 mlx;                          // DCQCN状态
    DataRate m_rate;                  // 当前发送速率
    Ptr<QbbNetDevice> dev;            // 输出设备
    bool dcqcn_initialized;           // DCQCN是否已初始化
    
    // 构造函数，初始化DCQCN状态
    VoqEntry() : voqId(0), currentSize(0), maxSize(0), isAllocated(false), 
                lastTransmitTime(Seconds(0)), 
                 m_rate(DataRate(0)), dcqcn_initialized(false) {
        // 初始化DCQCN状态
        mlx.m_alpha = 1.0;
        mlx.m_alpha_cnp_arrived = false;
        mlx.m_first_cnp = true;
        mlx.m_decrease_cnp_arrived = false;
        mlx.m_rpTimeStage = 0;
        mlx.m_targetRate = DataRate(0);
    }
};

// 调度表项
struct ScheduleEntry {
    Ipv4Address destIp;
    uint64_t rateLimit;                // 速率限制（bps）
    bool isCongested;                  // 拥塞状态标记
    Time lastUpdateTime;               // 最后更新时间
};

// 长距离传输状态
struct LongHaulState {
    uint64_t localSentBytes;
    uint64_t lastLocalSentBytes;
    uint64_t remoteForwardedBytes;
    uint64_t inflightBytes;
    uint64_t maxInflightBytes;
    uint32_t activeVoqCount;
    uint64_t maxVoqBytes;
    uint64_t maxTotalVoqBytes;
    uint64_t rateLimit;                // 速率限制（bps）
    uint64_t longHaulBandwidth;
    Time longHaulDelay;
    Time lastFeedbackTime;
    bool firstFeedback;
    CustomHeader longHaulCh;
    bool longHaulChFlag;
    std::unordered_set<uint32_t> longHaulSrcIps; // 远端源IP地址
};




class atcGateway : public Object
{
public:
    int m_gatewayStatus; // 0 未初始化，1 已启动，2 已停止
    int m_gatewayType; // 0普通节点 ，1 发送端门廊，2 接收到门廊
    bool m_congested; // 是否拥塞
    uint8_t m_ecnbits; // 报文 ECN 位
    uint32_t m_packet_size; // 报文净荷大小
    
    // ========== VOQ相关成员变量 ==========
    uint32_t m_maxVoqCount;             // 最大VOQ数量
    uint64_t m_maxVoqRate;              // 每个VOQ最大速率（bps）
    std::vector<VoqEntry> voqPool;                    // VOQ池
    std::map<Ipv4Address, uint32_t> destToVoqMap;     // 目的地→VOQID映射
    std::map<Ipv4Address, ScheduleEntry> scheduleTable; // 调度表
    
    // 保留原有的数据结构以保持兼容性
    Callback<void, Ptr<Packet>, CustomHeader&> m_sendCallBack;
    Callback<Ptr<QbbNetDevice>, Ptr<Packet>, CustomHeader&> m_getOutDevCallBack;
    std::unordered_map<uint32_t, Time> cnpNotifyTime; // 记录每个IP最近一次CNP通知的时间

    // 长距离控制
    LongHaulState longHaulState;
    EventId longHaulFeedbackEvent;

    int m_debug;

    /******************************
	 * Mellanox's version of DCQCN 参数
	 *****************************/
	double m_g; //feedback weight
	double m_rateOnFirstCNP; // the fraction of line rate to set on first CNP
	bool m_EcnClampTgtRate;
	double m_rpgTimeReset;
	double m_rateDecreaseInterval;
	uint32_t m_rpgThreshold;
	double m_alpha_resume_interval;
	DataRate m_rai;		//< Rate of additive increase
	DataRate m_rhai;		//< Rate of hyper-additive increase
    DataRate m_minRate;		//< Min sending rate

    /******************************
	 * Mellanox's version of DCQCN (VOQ版本)
	 *****************************/
    void UpdateAlphaMlxVoq(VoqEntry *voq);
    void ScheduleUpdateAlphaMlxVoq(VoqEntry *voq);
    void cnp_received_mlx_voq(VoqEntry *voq);
    void CheckRateDecreaseMlxVoq(VoqEntry *voq);
    void ScheduleDecreaseRateMlxVoq(VoqEntry *voq, uint32_t delta);
    void RateIncEventTimerMlxVoq(VoqEntry *voq);
    void RateIncEventMlxVoq(VoqEntry *voq);
    void FastRecoveryMlxVoq(VoqEntry *voq);
    void ActiveIncreaseMlxVoq(VoqEntry *voq);
    void HyperIncreaseMlxVoq(VoqEntry *voq);

    atcGateway();
    ~atcGateway();

    /**
     * @brief 门廊网关初始化
     */
    void init(int type,uint32_t max_voq_count,uint64_t max_voq_rate, uint64_t longHaulBandwidth,Time longHaulDelay);
    void setSwitchSendToDevCallback(Callback<void, Ptr<Packet>, CustomHeader&> cb);
    void DoSwitchSendToDev(Ptr<Packet> p, CustomHeader& ch);
    void setGetOutDevCallback(Callback<Ptr<QbbNetDevice>, Ptr<Packet>, CustomHeader&> cb);
    Ptr<QbbNetDevice> GetOutDevice(Ptr<Packet> p, CustomHeader& ch);

    uint16_t EtherToPpp (uint16_t proto);

    // ========== VOQ管理函数 ==========
    uint32_t AllocateVoq(Ipv4Address destIp);
    void ReleaseVoq(uint32_t voqId);
    void CheckVoqRelease(uint32_t voqId);
    Time CalculateTransmitTime(uint32_t pktSize, DataRate rate);
    void TransmitFromVoq(uint32_t voqId);
    void ScheduleVoqForward(uint32_t voqId);
    // VOQ统计
    void VoqStat();
    
    // ========== 调度表管理 ==========
    ScheduleEntry GetOrCreateScheduleEntry(Ipv4Address destIp);
    
    // ========== 数据包处理函数 ==========
    bool HandleDataPacketWithVoq(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch);
    bool HandleAckPacketWithVoq(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch);
    // 处理LongHaulFeedback
    bool HandleLongHaulFeedback(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch);

    // 初始化长距离状态
    void InitLongHaulState(uint64_t longHaulBandwidth,Time longHaulDelay);
    // 启动长距离反馈定时器
    void StartLongHaulFeedbackTimer();
     // 生成长距离反馈
    void GenerateLongHaulFeedback();
    // 限流本地转发速率
    void ThrottleLocalForwarding(Ptr<NetDevice> dev, uint64_t rate);

    // ========== 保留的原有接口（兼容性） ==========
    void sendCnp(CustomHeader &ch, Ptr<NetDevice> ingressDev);

    // main function
    // Used in switch-node.cc SendToDev()
    // return false if continue; return true if do not continue to send;
    /**
     * @brief 门廊报文处理
     * 
     * @param device 
     * @param pkt 报文
     * @param ch 自定义头
     */
    bool packetIn(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch);
};

}

#endif