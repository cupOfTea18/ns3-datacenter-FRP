// Shared route inspection used by R5 wiring and all-algorithm measurements.
namespace LonghaulRouting {
Ptr<Packet> RoutePacket(Ipv4Address src,Ipv4Address dst,uint8_t protocol) {
    auto p=Create<Packet>(); Ipv4Header ip; ip.SetSource(src); ip.SetDestination(dst);
    ip.SetProtocol(protocol); ip.SetTtl(64); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp); return p;
}
uint32_t Peer(Ptr<QbbNetDevice> dev) {
    auto ch=dev->GetChannel();
    return ch->GetDevice(0)==dev ? ch->GetDevice(1)->GetNode()->GetId() : ch->GetDevice(0)->GetNode()->GetId();
}
std::vector<uint32_t> TracePath(uint32_t start,uint32_t end,Ptr<Packet> p,CustomHeader h) {
    std::vector<uint32_t> path{start};
    while (path.back()!=end) {
        auto node=n.Get(path.back()); Ptr<QbbNetDevice> dev;
        if (!node->GetNodeType()) {
            NS_ABORT_MSG_IF(nbr2if[node].size()!=1,"multihomed host needs explicit route selection");
            dev=DynamicCast<QbbNetDevice>(node->GetDevice(nbr2if[node].begin()->second.idx));
        } else {
            int port=DynamicCast<SwitchNode>(node)->LookupOutputPort(p,h);
            NS_ABORT_MSG_IF(port<0,"managed path has no route");
            dev=DynamicCast<QbbNetDevice>(node->GetDevice(port));
        }
        uint32_t next=Peer(dev);
        NS_ABORT_MSG_IF(std::find(path.begin(),path.end(),next)!=path.end(),"managed route loop");
        path.push_back(next);
    }
    return path;
}
double PathDelay(const std::vector<uint32_t>& path,uint32_t bytes) {
    Time delay(0);
    for (size_t i=1;i<path.size();++i) {
        auto dev=DynamicCast<QbbNetDevice>(n.Get(path[i-1])->GetDevice(nbr2if[n.Get(path[i-1])].at(n.Get(path[i])).idx));
        auto peer=DynamicCast<QbbNetDevice>(n.Get(path[i])->GetDevice(nbr2if[n.Get(path[i])].at(n.Get(path[i-1])).idx));
        delay+=DynamicCast<QbbChannel>(dev->GetChannel())->GetDelay()+peer->GetReceiveDelay()+dev->GetDataRate().CalculateBytesTxTime(bytes);
    }
    return delay.GetSeconds();
}
}
