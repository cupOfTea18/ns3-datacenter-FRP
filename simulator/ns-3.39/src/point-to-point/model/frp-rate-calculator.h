/*
 * FRP (Fair Rate Protocol) 公平速率计算器
 * 
 * 核心公式: F_new = F_old - α*(q_cur - q_ref + lanbackoff) - β*(q_cur - q_old)
 * 
 * 量纲说明:
 *   - 内部计算: 使用600B Cell单位和10Mbps速率单位
 *   - 外部接口: 使用Byte和bps标准单位
 *   - 自动转换: 计算器内部处理所有量纲转换
 */

#ifndef FRP_RATE_CALCULATOR_H
#define FRP_RATE_CALCULATOR_H

#include "ns3/core-module.h"
#include <map>

namespace ns3 {

/**
 * FRP端口状态
 * 存储每个端口的公平速率计算状态
 */
struct FrpPortState {
    double currentFairRateBps;  // 当前公平速率 (单位: bps)
    double qOldBytes;           // 上一周期队列深度 (单位: Byte)
    bool isInitialized;         // 是否已初始化

    FrpPortState() : currentFairRateBps(0.0), qOldBytes(0.0), isInitialized(false) {}
};

/**
 * FRP公平速率计算器
 * 负责维护每个端口的状态并计算公平速率
 */
class FrpRateCalculator {
public:
    FrpRateCalculator();
    ~FrpRateCalculator();

    /**
     * 计算公平速率
     *
     * @param portId 端口号
     * @param linkBps 链路带宽 (单位: bps)
     * @param currentQBytes 当前队列深度 (单位: Byte)
     * @param hasWanFlow 是否有广域流
     * @param ccMode 拥塞控制模式 (13=FRP, 14=ROCC)
     * @param consecutiveCrossDcTrigger 上游统计的"跨域流连续识别"计数 (由调用方负责维护)
     * @return 公平速率 (单位: bps)
     */
    double CalculateFairRate(uint32_t portId, uint64_t linkBps, uint32_t currentQBytes, bool hasWanFlow, uint8_t ccMode, uint8_t consecutiveCrossDcTrigger);

    /**
     * 重置指定端口状态
     */
    void ResetPort(uint32_t portId);

    /**
     * 重置所有端口状态
     */
    void ResetAll();

private:
    std::map<uint32_t, FrpPortState> m_portStates;

    // FRP算法参数 (全局标准量纲)
    double m_alpha;              // α系数 (无量纲)
    double m_beta;               // β系数 (无量纲)
    double m_scaleFactor;        // scaleFactor (无量纲，值为6)
    double m_tPeriodS;           // 反馈周期 (单位: 秒)
    
    // 队列阈值和参考值 (单位: Byte)
    double m_qThBytes;           // q_th阈值
    double m_qRef100GBytes;      // 100Gbps时的q_ref
    double m_qRef200GBytes;      // 200Gbps时的q_ref
};

} // namespace ns3

#endif // FRP_RATE_CALCULATOR_H
