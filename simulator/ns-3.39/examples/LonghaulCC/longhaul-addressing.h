#ifndef LONGHAUL_ADDRESSING_H
#define LONGHAUL_ADDRESSING_H
#include "ns3/ipv4-address.h"
#include "ns3/abort.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace LonghaulAddressing {
struct Link {
    uint32_t src, dst;
    std::string rate, delay;
    double error;
};
struct Plan {
    std::vector<uint32_t> dc, leaf;
    std::map<std::pair<uint32_t,uint32_t>, ns3::Ipv4Address> interfaces;
};
// No workload input. Device installation order remains the topology-file order.
inline Plan Build(const std::vector<uint32_t>& types, const std::vector<Link>& links,
                  uint32_t left, uint32_t right) {
    using namespace ns3;
    NS_ABORT_MSG_IF(left>=types.size() || right>=types.size() || left==right ||
                    !types[left] || !types[right], "invalid DCI endpoints");
    Plan plan;
    plan.dc.resize(types.size()); plan.leaf.assign(types.size(), UINT32_MAX);
    std::vector<std::vector<uint32_t>> adjacent(types.size());
    std::set<std::pair<uint32_t,uint32_t>> edges;
    for (const auto& l:links) {
        NS_ABORT_MSG_IF(l.src>=types.size() || l.dst>=types.size() || l.src==l.dst ||
            !edges.insert(std::minmax(l.src,l.dst)).second, "invalid/parallel topology edge");
        adjacent[l.src].push_back(l.dst); adjacent[l.dst].push_back(l.src);
    }
    NS_ABORT_MSG_IF(!edges.count(std::minmax(left,right)), "missing managed WAN edge");
    for (uint32_t root:{left,right}) {
        const uint32_t dc=root==left ? 1 : 2;
        NS_ABORT_MSG_IF(plan.dc[root], "WAN removal must separate the two DCs");
        std::vector<uint32_t> todo{root}; plan.dc[root]=dc;
        for (size_t k=0;k<todo.size();++k) for (auto next:adjacent[todo[k]]) {
            if (std::minmax(todo[k],next)==std::minmax(left,right)) continue;
            NS_ABORT_MSG_IF(plan.dc[next] && plan.dc[next]!=dc, "overlapping DC components");
            if (!plan.dc[next]) { plan.dc[next]=dc; todo.push_back(next); }
        }
    }
    std::map<uint32_t,std::vector<uint32_t>> hosts;
    for (uint32_t id=0;id<types.size();++id) {
        NS_ABORT_MSG_IF(!plan.dc[id], "unconnected topology component");
        if (types[id]) continue;
        NS_ABORT_MSG_IF(adjacent[id].size()!=1 || types[adjacent[id][0]]!=1,
                        "each host must have one access Leaf");
        plan.leaf[id]=adjacent[id][0]; hosts[plan.leaf[id]].push_back(id);
    }
    uint32_t indices[3]={0,0,0};
    for (const auto& e:hosts) {
        auto leaf=e.first, dc=plan.dc[leaf], index=++indices[dc];
        NS_ABORT_MSG_IF(index>254 || e.second.size()>127, "Leaf address pool exhausted");
        uint32_t base=0x0a000000 | (dc<<16) | (index<<8);
        for (uint32_t h=0;h<e.second.size();++h) {
            plan.interfaces[{e.second[h],leaf}]=Ipv4Address(base+h+1);
            plan.interfaces[{leaf,e.second[h]}]=Ipv4Address(base+254-h);
        }
    }
    uint32_t transit=0;
    for (auto edge:edges) if (types[edge.first] && types[edge.second]) {
        NS_ABORT_MSG_IF(transit>=16384, "transit address pool exhausted");
        uint32_t base=0x0a030000+4*transit++;
        plan.interfaces[edge]=Ipv4Address(base+1);
        plan.interfaces[{edge.second,edge.first}]=Ipv4Address(base+2);
    }
    std::set<uint32_t> unique;
    for (const auto& e:plan.interfaces)
        NS_ABORT_MSG_IF(!unique.insert(e.second.Get()).second, "duplicate interface IP");
    NS_ABORT_MSG_IF(plan.interfaces.size()!=2*links.size(), "unassigned interface");
    return plan;
}
}
#endif
