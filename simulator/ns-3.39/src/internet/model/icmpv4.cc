/*
 * Copyright (c) 2008 INRIA
 *
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
 *
 * Author: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 */

#include "icmpv4.h"

#include "ns3/log.h"
#include "ns3/packet.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("Icmpv4Header");

/********************************************************
 *        Icmpv4Header
 ********************************************************/

NS_OBJECT_ENSURE_REGISTERED(Icmpv4Header);

TypeId
Icmpv4Header::GetTypeId()
{
    static TypeId tid = TypeId("ns3::Icmpv4Header")
                            .SetParent<Header>()
                            .SetGroupName("Internet")
                            .AddConstructor<Icmpv4Header>();
    return tid;
}

Icmpv4Header::Icmpv4Header()
    : m_type(0),
      m_code(0),
      m_calcChecksum(false)
{
    NS_LOG_FUNCTION(this);
}

Icmpv4Header::~Icmpv4Header()
{
    NS_LOG_FUNCTION(this);
}

void
Icmpv4Header::EnableChecksum()
{
    NS_LOG_FUNCTION(this);
    m_calcChecksum = true;
}

TypeId
Icmpv4Header::GetInstanceTypeId() const
{
    NS_LOG_FUNCTION(this);
    return GetTypeId();
}

uint32_t
Icmpv4Header::GetSerializedSize() const
{
    NS_LOG_FUNCTION(this);
    return 4;
}

void
Icmpv4Header::Serialize(Buffer::Iterator start) const
{
    NS_LOG_FUNCTION(this << &start);
    Buffer::Iterator i = start;
    i.WriteU8(m_type);
    i.WriteU8(m_code);
    i.WriteHtonU16(0);
    if (m_calcChecksum)
    {
        i = start;
        uint16_t checksum = i.CalculateIpChecksum(i.GetSize());
        i = start;
        i.Next(2);
        i.WriteU16(checksum);
    }
}

uint32_t
Icmpv4Header::Deserialize(Buffer::Iterator start)
{
    NS_LOG_FUNCTION(this << &start);
    m_type = start.ReadU8();
    m_code = start.ReadU8();
    start.Next(2); // uint16_t checksum = start.ReadNtohU16 ();
    return 4;
}

void
Icmpv4Header::Print(std::ostream& os) const
{
    NS_LOG_FUNCTION(this << &os);
    os << "type=" << (uint32_t)m_type << ", code=" << (uint32_t)m_code;
}

void
Icmpv4Header::SetType(uint8_t type)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(type));
    m_type = type;
}

void
Icmpv4Header::SetCode(uint8_t code)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(code));
    m_code = code;
}

uint8_t
Icmpv4Header::GetType() const
{
    NS_LOG_FUNCTION(this);
    return m_type;
}

uint8_t
Icmpv4Header::GetCode() const
{
    NS_LOG_FUNCTION(this);
    return m_code;
}

/********************************************************
 *        Icmpv4Echo
 ********************************************************/

NS_OBJECT_ENSURE_REGISTERED(Icmpv4Echo);

void
Icmpv4Echo::SetIdentifier(uint16_t id)
{
    NS_LOG_FUNCTION(this << id);
    m_identifier = id;
}

void
Icmpv4Echo::SetSequenceNumber(uint16_t seq)
{
    NS_LOG_FUNCTION(this << seq);
    m_sequence = seq;
}

void
Icmpv4Echo::SetData(Ptr<const Packet> data)
{
    NS_LOG_FUNCTION(this << *data);

    uint32_t size = data->GetSize();
    //
    // All kinds of optimizations are possible, but let's not get carried away
    // since this is probably a very uncommon thing in the big picture.
    //
    // N.B. Zero is a legal size for the alloc below even though a hardcoded zero
    // would result in  warning.
    //
    if (size != m_dataSize)
    {
        delete[] m_data;
        m_data = new uint8_t[size];
        m_dataSize = size;
    }
    data->CopyData(m_data, size);
}

uint16_t
Icmpv4Echo::GetIdentifier() const
{
    NS_LOG_FUNCTION(this);
    return m_identifier;
}

uint16_t
Icmpv4Echo::GetSequenceNumber() const
{
    NS_LOG_FUNCTION(this);
    return m_sequence;
}

uint32_t
Icmpv4Echo::GetDataSize() const
{
    NS_LOG_FUNCTION(this);
    return m_dataSize;
}

uint32_t
Icmpv4Echo::GetData(uint8_t payload[]) const
{
    NS_LOG_FUNCTION(this << payload);
    memcpy(payload, m_data, m_dataSize);
    return m_dataSize;
}

TypeId
Icmpv4Echo::GetTypeId()
{
    static TypeId tid = TypeId("ns3::Icmpv4Echo")
                            .SetParent<Header>()
                            .SetGroupName("Internet")
                            .AddConstructor<Icmpv4Echo>();
    return tid;
}

Icmpv4Echo::Icmpv4Echo()
    : m_identifier(0),
      m_sequence(0),
      m_dataSize(0)
{
    NS_LOG_FUNCTION(this);
    //
    // After construction, m_data is always valid until destruction.  This is true
    // even if m_dataSize is zero.
    //
    m_data = new uint8_t[m_dataSize];
}

Icmpv4Echo::~Icmpv4Echo()
{
    NS_LOG_FUNCTION(this);
    delete[] m_data;
    m_data = nullptr;
    m_dataSize = 0;
}

TypeId
Icmpv4Echo::GetInstanceTypeId() const
{
    NS_LOG_FUNCTION(this);
    return GetTypeId();
}

uint32_t
Icmpv4Echo::GetSerializedSize() const
{
    NS_LOG_FUNCTION(this);
    return 4 + m_dataSize;
}

void
Icmpv4Echo::Serialize(Buffer::Iterator start) const
{
    NS_LOG_FUNCTION(this << &start);
    start.WriteHtonU16(m_identifier);
    start.WriteHtonU16(m_sequence);
    start.Write(m_data, m_dataSize);
}

uint32_t
Icmpv4Echo::Deserialize(Buffer::Iterator start)
{
    NS_LOG_FUNCTION(this << &start);

    uint32_t optionalPayloadSize = start.GetRemainingSize() - 4;
    NS_ASSERT(start.GetRemainingSize() >= 4);

    m_identifier = start.ReadNtohU16();
    m_sequence = start.ReadNtohU16();
    if (optionalPayloadSize != m_dataSize)
    {
        delete[] m_data;
        m_dataSize = optionalPayloadSize;
        m_data = new uint8_t[m_dataSize];
    }
    start.Read(m_data, m_dataSize);
    return m_dataSize + 4;
}

void
Icmpv4Echo::Print(std::ostream& os) const
{
    NS_LOG_FUNCTION(this << &os);
    os << "identifier=" << m_identifier << ", sequence=" << m_sequence
       << ", data size=" << m_dataSize;
}

/********************************************************
 *        Icmpv4DestinationUnreachable
 ********************************************************/

NS_OBJECT_ENSURE_REGISTERED(Icmpv4DestinationUnreachable);

TypeId
Icmpv4DestinationUnreachable::GetTypeId()
{
    static TypeId tid = TypeId("ns3::Icmpv4DestinationUnreachable")
                            .SetParent<Header>()
                            .SetGroupName("Internet")
                            .AddConstructor<Icmpv4DestinationUnreachable>();
    return tid;
}

Icmpv4DestinationUnreachable::Icmpv4DestinationUnreachable()
{
    NS_LOG_FUNCTION(this);
    // make sure that thing is initialized to get initialized bytes
    // when the ip payload's size is smaller than 8 bytes.
    for (uint8_t j = 0; j < 8; j++)
    {
        m_data[j] = 0;
    }
}

void
Icmpv4DestinationUnreachable::SetNextHopMtu(uint16_t mtu)
{
    NS_LOG_FUNCTION(this << mtu);
    m_nextHopMtu = mtu;
}

uint16_t
Icmpv4DestinationUnreachable::GetNextHopMtu() const
{
    NS_LOG_FUNCTION(this);
    return m_nextHopMtu;
}

void
Icmpv4DestinationUnreachable::SetData(Ptr<const Packet> data)
{
    NS_LOG_FUNCTION(this << *data);
    data->CopyData(m_data, 8);
}

void
Icmpv4DestinationUnreachable::SetHeader(Ipv4Header header)
{
    NS_LOG_FUNCTION(this << header);
    m_header = header;
}

void
Icmpv4DestinationUnreachable::GetData(uint8_t payload[8]) const
{
    NS_LOG_FUNCTION(this << payload);
    memcpy(payload, m_data, 8);
}

Ipv4Header
Icmpv4DestinationUnreachable::GetHeader() const
{
    NS_LOG_FUNCTION(this);
    return m_header;
}

Icmpv4DestinationUnreachable::~Icmpv4DestinationUnreachable()
{
}

TypeId
Icmpv4DestinationUnreachable::GetInstanceTypeId() const
{
    NS_LOG_FUNCTION(this);
    return GetTypeId();
}

uint32_t
Icmpv4DestinationUnreachable::GetSerializedSize() const
{
    NS_LOG_FUNCTION(this);
    return 4 + m_header.GetSerializedSize() + 8;
}

void
Icmpv4DestinationUnreachable::Serialize(Buffer::Iterator start) const
{
    NS_LOG_FUNCTION(this << &start);
    start.WriteU16(0);
    start.WriteHtonU16(m_nextHopMtu);
    uint32_t size = m_header.GetSerializedSize();
    m_header.Serialize(start);
    start.Next(size);
    start.Write(m_data, 8);
}

uint32_t
Icmpv4DestinationUnreachable::Deserialize(Buffer::Iterator start)
{
    NS_LOG_FUNCTION(this << &start);
    Buffer::Iterator i = start;
    i.Next(2);
    m_nextHopMtu = i.ReadNtohU16();
    uint32_t read = m_header.Deserialize(i);
    i.Next(read);
    for (uint8_t j = 0; j < 8; j++)
    {
        m_data[j] = i.ReadU8();
    }
    return i.GetDistanceFrom(start);
}

void
Icmpv4DestinationUnreachable::Print(std::ostream& os) const
{
    NS_LOG_FUNCTION(this << &os);
    m_header.Print(os);
    os << " org data=";
    for (uint8_t i = 0; i < 8; i++)
    {
        os << (uint32_t)m_data[i];
        if (i != 8)
        {
            os << " ";
        }
    }
}

/********************************************************
 *        Icmpv4TimeExceeded
 ********************************************************/

NS_OBJECT_ENSURE_REGISTERED(Icmpv4TimeExceeded);

TypeId
Icmpv4TimeExceeded::GetTypeId()
{
    static TypeId tid = TypeId("ns3::Icmpv4TimeExceeded")
                            .SetParent<Header>()
                            .SetGroupName("Internet")
                            .AddConstructor<Icmpv4TimeExceeded>();
    return tid;
}

Icmpv4TimeExceeded::Icmpv4TimeExceeded()
{
    NS_LOG_FUNCTION(this);
    // make sure that thing is initialized to get initialized bytes
    // when the ip payload's size is smaller than 8 bytes.
    for (uint8_t j = 0; j < 8; j++)
    {
        m_data[j] = 0;
    }
}

void
Icmpv4TimeExceeded::SetData(Ptr<const Packet> data)
{
    NS_LOG_FUNCTION(this << *data);
    data->CopyData(m_data, 8);
}

void
Icmpv4TimeExceeded::SetHeader(Ipv4Header header)
{
    NS_LOG_FUNCTION(this << header);
    m_header = header;
}

void
Icmpv4TimeExceeded::GetData(uint8_t payload[8]) const
{
    NS_LOG_FUNCTION(this << payload);
    memcpy(payload, m_data, 8);
}

Ipv4Header
Icmpv4TimeExceeded::GetHeader() const
{
    NS_LOG_FUNCTION(this);
    return m_header;
}

Icmpv4TimeExceeded::~Icmpv4TimeExceeded()
{
    NS_LOG_FUNCTION(this);
}

TypeId
Icmpv4TimeExceeded::GetInstanceTypeId() const
{
    NS_LOG_FUNCTION(this);
    return GetTypeId();
}

uint32_t
Icmpv4TimeExceeded::GetSerializedSize() const
{
    NS_LOG_FUNCTION(this);
    return 4 + m_header.GetSerializedSize() + 8;
}

void
Icmpv4TimeExceeded::Serialize(Buffer::Iterator start) const
{
    NS_LOG_FUNCTION(this << &start);
    start.WriteU32(0);
    uint32_t size = m_header.GetSerializedSize();
    m_header.Serialize(start);
    start.Next(size);
    start.Write(m_data, 8);
}

uint32_t
Icmpv4TimeExceeded::Deserialize(Buffer::Iterator start)
{
    NS_LOG_FUNCTION(this << &start);
    Buffer::Iterator i = start;
    i.Next(4);
    uint32_t read = m_header.Deserialize(i);
    i.Next(read);
    for (uint8_t j = 0; j < 8; j++)
    {
        m_data[j] = i.ReadU8();
    }
    return i.GetDistanceFrom(start);
}

void
Icmpv4TimeExceeded::Print(std::ostream& os) const
{
    NS_LOG_FUNCTION(this << &os);
    m_header.Print(os);
    os << " org data=";
    for (uint8_t i = 0; i < 8; i++)
    {
        os << (uint32_t)m_data[i];
        if (i != 8)
        {
            os << " ";
        }
    }
}

/********************************************************
 *        Icmpv4FrpFeedback
 ********************************************************/

NS_OBJECT_ENSURE_REGISTERED(Icmpv4FrpFeedback);

TypeId
Icmpv4FrpFeedback::GetTypeId()
{
    static TypeId tid = TypeId("ns3::Icmpv4FrpFeedback")
                            .SetParent<Header>()
                            .SetGroupName("Internet")
                            .AddConstructor<Icmpv4FrpFeedback>();
    return tid;
}

Icmpv4FrpFeedback::Icmpv4FrpFeedback()
    : m_fairRate(0),
      m_qDepth(0),
      m_cpId(0),
      m_type(false),
      m_linkRate(0)
{
    NS_LOG_FUNCTION(this);
}

Icmpv4FrpFeedback::~Icmpv4FrpFeedback()
{
    NS_LOG_FUNCTION(this);
}

TypeId
Icmpv4FrpFeedback::GetInstanceTypeId() const
{
    NS_LOG_FUNCTION(this);
    return GetTypeId();
}

uint32_t
Icmpv4FrpFeedback::GetSerializedSize() const
{
    NS_LOG_FUNCTION(this);
    // 16位(fair_rate) + 16位(q_depth) + 16位(cp_id) + 16位(has_wan + link_rate) = 64 bits = 8 字节
    // 完美契合 P4 的 FRP_shim_t 结构，不扩容，极度节约资源！
    return 8;
}

void
Icmpv4FrpFeedback::Serialize(Buffer::Iterator start) const
{
    NS_LOG_FUNCTION(this << &start);
    Buffer::Iterator i = start;

    // 写入 fairRate (16位，网络字节序) - 单位：10Mbps
    i.WriteHtonU16(m_fairRate);
    
    // 写入 qDepth (16位，网络字节序) - 单位：600B块 (int16_t, 可为负值)
    i.WriteHtonU16(static_cast<uint16_t>(m_qDepth));
    
    // 写入 cpId (16位，网络字节序)
    i.WriteHtonU16(m_cpId);
    
    // 【核心位操作】组装最后的 16 位：最高位是 type，低 15 位是 link_rate
    uint16_t last16Bits = 0;
    if (m_type) {
        last16Bits |= (1 << 15);  // 将 type 写入第 15 位 (MSB)
    }
    // 限制 m_linkRate 必须在 15 位以内 (0 ~ 32767)，并入低 15 位
    last16Bits |= (m_linkRate & 0x7FFF);
    
    i.WriteHtonU16(last16Bits);
}

uint32_t
Icmpv4FrpFeedback::Deserialize(Buffer::Iterator start)
{
    NS_LOG_FUNCTION(this << &start);
    Buffer::Iterator i = start;

    // 读取 fairRate (16位，网络字节序)
    m_fairRate = i.ReadNtohU16();
    
    // 读取 qDepth (16位，网络字节序) - int16_t, 可为负值
    m_qDepth = static_cast<int16_t>(i.ReadNtohU16());
    
    // 读取 cpId (16位，网络字节序)
    m_cpId = i.ReadNtohU16();
    
    // 【核心位操作解包】
    uint16_t last16Bits = i.ReadNtohU16();
    m_type     = (last16Bits >> 15) & 0x01;  // 提取最高位
    m_linkRate = last16Bits & 0x7FFF;        // 提取低 15 位 (得到以 10Mbps 为单位的带宽)

    return 8; // 保持 8 字节不变
}

void
Icmpv4FrpFeedback::Print(std::ostream& os) const
{
    NS_LOG_FUNCTION(this << &os);
    os << "fair_rate=" << m_fairRate 
       << "x10Mbps, q_depth=" << m_qDepth 
       << " blocks(600B), cp_id=" << m_cpId
       << ", type=" << (m_type ? "ROCC" : "FRP")
       << ", link_rate=" << m_linkRate << "x10Mbps";
}

// ====================================================================
// Setters & Getters 实现
// ====================================================================

void Icmpv4FrpFeedback::SetFairRate(uint16_t fairRate) {
    NS_LOG_FUNCTION(this << fairRate);
    m_fairRate = fairRate;
}

void Icmpv4FrpFeedback::SetQDepth(int16_t qDepth) {
    NS_LOG_FUNCTION(this << qDepth);
    m_qDepth = qDepth;
}

void Icmpv4FrpFeedback::SetCpId(uint16_t cpId) {
    NS_LOG_FUNCTION(this << cpId);
    m_cpId = cpId;
}

void Icmpv4FrpFeedback::SetType(bool type) {
    NS_LOG_FUNCTION(this << type);
    m_type = type;
}

void Icmpv4FrpFeedback::SetLinkRate(uint16_t linkRate) {
    NS_LOG_FUNCTION(this << linkRate);
    m_linkRate = linkRate;
}

uint16_t Icmpv4FrpFeedback::GetFairRate() const  {
    NS_LOG_FUNCTION(this); 
    return m_fairRate; 
}

int16_t Icmpv4FrpFeedback::GetQDepth() const    {
    NS_LOG_FUNCTION(this); 
    return m_qDepth; 
}

uint16_t Icmpv4FrpFeedback::GetCpId() const      {
    NS_LOG_FUNCTION(this); 
    return m_cpId; 
}

bool Icmpv4FrpFeedback::GetType() const    {
    NS_LOG_FUNCTION(this); 
    return m_type; 
}

uint16_t Icmpv4FrpFeedback::GetLinkRate() const {
    NS_LOG_FUNCTION(this);
    return m_linkRate;
}

} // namespace ns3
