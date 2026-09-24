#!/usr/bin/env python3
"""Validate the fixed two-DC topology used by the long-haul baseline."""

from __future__ import annotations

import argparse
from collections import defaultdict, deque
from pathlib import Path


def rate_bps(value: str) -> float:
    units = {"bps": 1, "kbps": 1e3, "mbps": 1e6, "gbps": 1e9, "tbps": 1e12}
    value = value.lower()
    for unit, multiplier in units.items():
        if value.endswith(unit):
            return float(value[: -len(unit)]) * multiplier
    return float(value)


def delay_ns(value: str) -> float:
    units = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}
    value = value.lower()
    for unit, multiplier in units.items():
        if value.endswith(unit):
            return float(value[: -len(unit)]) * multiplier
    raise ValueError(f"delay has no unit: {value}")


def connected(start: int, graph: dict[int, set[int]], blocked: frozenset[int] = frozenset()) -> set[int]:
    seen = {start}
    queue = deque([start])
    while queue:
        node = queue.popleft()
        for neighbor in graph[node]:
            if frozenset((node, neighbor)) == blocked:
                continue
            if neighbor not in seen:
                seen.add(neighbor)
                queue.append(neighbor)
    return seen


def shortest_delay(src: int, dst: int, graph: dict[int, list[tuple[int, float]]]) -> float:
    queue = deque([(src, 0.0)])
    seen = {src}
    while queue:
        node, distance = queue.popleft()
        if node == dst:
            return distance
        for neighbor, edge_delay in graph[node]:
            if neighbor not in seen:
                seen.add(neighbor)
                queue.append((neighbor, distance + edge_delay))
    raise ValueError(f"no path from {src} to {dst}")


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def main() -> int:
    parser = argparse.ArgumentParser()
    default_topology = Path(__file__).resolve().parent / "topology-longhaul.txt"
    parser.add_argument("topology", nargs="?", default=str(default_topology))
    args = parser.parse_args()
    path = Path(args.topology)
    if not path.is_file():
        fail(f"topology does not exist: {path}")

    tokens = path.read_text().split()
    if len(tokens) < 4:
        fail("missing topology header")
    node_count, switch_count, tor_count, link_count = map(int, tokens[:4])
    offset = 4
    if len(tokens) < offset + switch_count + 5 * link_count:
        fail("topology ends before the declared switch/link records")
    switches = list(map(int, tokens[offset : offset + switch_count]))
    offset += switch_count
    if len(set(switches)) != switch_count:
        fail("switch list contains duplicates")
    if any(node < 0 or node >= node_count for node in switches):
        fail("switch list contains an out-of-range node")
    switch_set = set(switches)
    left_leaves = set(range(32, 36))
    left_spines = set(range(36, 40))
    left_gateway = {40}
    right_leaves = set(range(73, 77))
    right_spines = set(range(77, 81))
    right_gateway = {81}
    expected_switches = (left_leaves | left_spines | left_gateway |
                         right_leaves | right_spines | right_gateway)
    tors = set(switches[:tor_count])
    if (node_count, switch_count, tor_count, link_count) != (82, 18, 8, 105):
        fail("expected header 82 nodes, 18 switches, 8 leaves, 105 links, "
             f"got {node_count} nodes, {switch_count} switches, "
             f"{tor_count} leaves, {link_count} links")
    if switch_set != expected_switches:
        fail(f"unexpected switch IDs: {sorted(switch_set)}")
    if tors != left_leaves | right_leaves:
        fail(f"expected leaves 32..35 and 73..76, got {sorted(tors)}")

    links: list[tuple[int, int, float, float, float]] = []
    edge_set: set[frozenset[int]] = set()
    graph: dict[int, set[int]] = defaultdict(set)
    weighted: dict[int, list[tuple[int, float]]] = defaultdict(list)
    switch_graph: dict[int, set[int]] = defaultdict(set)
    host_graph: dict[int, set[int]] = defaultdict(set)
    for _ in range(link_count):
        src, dst = int(tokens[offset]), int(tokens[offset + 1])
        rate, delay, error = tokens[offset + 2 : offset + 5]
        offset += 5
        if src == dst:
            fail(f"self-loop at node {src}")
        if not (0 <= src < node_count and 0 <= dst < node_count):
            fail(f"out-of-range link {src} {dst}")
        edge = frozenset((src, dst))
        if edge in edge_set:
            fail(f"duplicate link {src} {dst}")
        edge_set.add(edge)
        link = (src, dst, rate_bps(rate), delay_ns(delay), float(error))
        links.append(link)
        graph[src].add(dst)
        graph[dst].add(src)
        weighted[src].append((dst, link[3]))
        weighted[dst].append((src, link[3]))
        if src in switch_set and dst in switch_set:
            switch_graph[src].add(dst)
            switch_graph[dst].add(src)
        else:
            host_graph[src].add(dst)
            host_graph[dst].add(src)
    if offset != len(tokens):
        fail("extra tokens after declared topology records")
    if len(links) != link_count:
        fail("link count mismatch")

    hosts = set(range(node_count)) - switch_set
    if len(hosts) != 64:
        fail(f"expected 64 hosts, got {len(hosts)}")
    if len(switch_set) != 18:
        fail(f"expected 18 switches, got {len(switch_set)}")
    if len(edge_set) != 105:
        fail(f"expected 105 unique links, got {len(edge_set)}")
    for host in hosts:
        if len(graph[host]) != 1:
            fail(f"host {host} has degree {len(graph[host])}, expected 1")
    for tor in tors:
        host_neighbors = graph[tor] & hosts
        if len(host_neighbors) != 8:
            fail(f"leaf {tor} has {len(host_neighbors)} attached hosts, expected 8")
        expected_spines = left_spines if tor in left_leaves else right_spines
        if graph[tor] & switch_set != expected_spines:
            fail(f"leaf {tor} is not connected to all four local spines")

    for spine in left_spines | right_spines:
        expected_leaves = left_leaves if spine in left_spines else right_leaves
        expected_gateway = left_gateway if spine in left_spines else right_gateway
        if graph[spine] != expected_leaves | expected_gateway:
            fail(f"spine {spine} must connect to four leaves and its gateway")
    if graph[40] != left_spines | {81}:
        fail("gateway 40 must connect to four local spines and gateway 81")
    if graph[81] != right_spines | {40}:
        fail("gateway 81 must connect to four local spines and gateway 40")

    dci_edges = [link for link in links if {link[0], link[1]} == {40, 81}]
    if len(dci_edges) != 1:
        fail(f"expected one 40<->81 link, got {len(dci_edges)}")
    dci = dci_edges[0]
    if dci[2] != 200e9 or dci[3] != 5e6 or dci[4] != 0:
        fail(f"bad DCI link: rate={dci[2]} delay_ns={dci[3]} error={dci[4]}")
    for src, dst, rate, delay, error in links:
        if {src, dst} != {40, 81} and (rate != 100e9 or delay != 1.5e3 or error != 0):
            fail(f"bad internal link {src} {dst}: rate={rate} delay_ns={delay} error={error}")

    if len(connected(32, graph)) != node_count:
        fail("topology is not connected")
    left_switches = connected(32, switch_graph, frozenset((40, 81)))
    right_switches = set(switch_set) - left_switches
    if left_switches != left_leaves | left_spines | left_gateway or right_switches != right_leaves | right_spines | right_gateway:
        fail("removing the DCI does not produce the expected two DC switch sets")
    if connected(32, graph, frozenset((40, 81))) & right_switches:
        fail("a cross-DC path exists without the DCI link")

    host_dc = {}
    for host in hosts:
        tor = next(iter(graph[host] & tors))
        host_dc[host] = 0 if tor in left_switches else 1
    if sum(dc == 0 for dc in host_dc.values()) != 32 or sum(dc == 1 for dc in host_dc.values()) != 32:
        fail("each DC must contain exactly 32 hosts")
    rtt_ns = 2 * shortest_delay(0, 41, weighted)
    if not 9e6 <= rtt_ns <= 11e6:
        fail(f"representative cross-DC RTT is {rtt_ns:.0f} ns, expected about 10 ms")

    print(f"OK: {path}")
    print(f"  nodes={node_count} switches={len(switch_set)} hosts={len(hosts)} links={len(links)}")
    print("  dc0_hosts=32 dc1_hosts=32 leaves=4 spines=4 gateways=1 "
          "dci=40<->81 rate=200Gbps one_way_delay=5ms")
    print(f"  representative_rtt={rtt_ns / 1e6:.6f}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
