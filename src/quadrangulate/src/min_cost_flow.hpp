// Min-cost max-flow (SPFA successive shortest paths). The reusable engine for
// the integer-parametrization Stage-2 solve (docs/integer-parametrization-plan.md):
// the integer optimization is a min-cost flow that balances per-face coordinate
// divergences by adjusting per-edge integer jumps. Clean-room; small graphs
// (~1e4 nodes for our corpus), so SPFA is fine.
//
// Extracted verbatim from quad_extract.cpp so the Bi-MDF quantizer
// (bimdf_quantize.cpp) can reuse it; behavior is unchanged. Parallel arcs are
// natively supported (adjacency is a per-node edge-id list), which is what
// lets convex piecewise-linear deviation costs decompose into parallel arcs
// with nondecreasing unit costs.
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace cyber::remesh::detail {

class MinCostFlow {
public:
    void init(int nodes) {
        n_ = nodes;
        adj_.assign(static_cast<std::size_t>(nodes), {});
        edges_.clear();
    }
    // Directed arc u->v with capacity cap and per-unit cost. Returns the index of
    // the forward arc (its flow is readable afterwards via flow()).
    int addEdge(int u, int v, int cap, int cost) {
        const int id = static_cast<int>(edges_.size());
        adj_[static_cast<std::size_t>(u)].push_back(id);
        edges_.push_back(Edge{v, cap, 0, cost});
        adj_[static_cast<std::size_t>(v)].push_back(id + 1);
        edges_.push_back(Edge{u, 0, 0, -cost});
        return id;
    }
    // Push max flow from s to t at minimum cost. Returns {flow, cost}.
    std::pair<int, int> solve(int s, int t) {
        int totalFlow = 0, totalCost = 0;
        while (true) {
            std::vector<int> dist(static_cast<std::size_t>(n_), kInf);
            std::vector<int> inQueue(static_cast<std::size_t>(n_), 0);
            std::vector<int> prevEdge(static_cast<std::size_t>(n_), -1);
            dist[static_cast<std::size_t>(s)] = 0;
            std::vector<int> queue{s};
            inQueue[static_cast<std::size_t>(s)] = 1;
            std::size_t head = 0;
            while (head < queue.size()) {
                const int u = queue[head++];
                inQueue[static_cast<std::size_t>(u)] = 0;
                for (const int id : adj_[static_cast<std::size_t>(u)]) {
                    const Edge& e = edges_[static_cast<std::size_t>(id)];
                    if (e.cap - e.flow <= 0 || dist[static_cast<std::size_t>(u)] == kInf) {
                        continue;
                    }
                    const int nd = dist[static_cast<std::size_t>(u)] + e.cost;
                    if (nd < dist[static_cast<std::size_t>(e.to)]) {
                        dist[static_cast<std::size_t>(e.to)] = nd;
                        prevEdge[static_cast<std::size_t>(e.to)] = id;
                        if (!inQueue[static_cast<std::size_t>(e.to)]) {
                            inQueue[static_cast<std::size_t>(e.to)] = 1;
                            queue.push_back(e.to);
                        }
                    }
                }
            }
            if (dist[static_cast<std::size_t>(t)] == kInf) {
                break;  // no augmenting path
            }
            int push = kInf;
            for (int v = t; v != s;) {
                const Edge& e =
                    edges_[static_cast<std::size_t>(prevEdge[static_cast<std::size_t>(v)])];
                push = std::min(push, e.cap - e.flow);
                v = edges_[static_cast<std::size_t>(prevEdge[static_cast<std::size_t>(v)] ^ 1)].to;
            }
            for (int v = t; v != s;) {
                const int id = prevEdge[static_cast<std::size_t>(v)];
                edges_[static_cast<std::size_t>(id)].flow += push;
                edges_[static_cast<std::size_t>(id ^ 1)].flow -= push;
                v = edges_[static_cast<std::size_t>(id ^ 1)].to;
            }
            totalFlow += push;
            totalCost += push * dist[static_cast<std::size_t>(t)];
        }
        return {totalFlow, totalCost};
    }
    [[nodiscard]] int flow(int forwardEdgeId) const {
        return edges_[static_cast<std::size_t>(forwardEdgeId)].flow;
    }

private:
    struct Edge {
        int to, cap, flow, cost;
    };
    static constexpr int kInf = 1 << 30;
    int n_ = 0;
    std::vector<Edge> edges_;
    std::vector<std::vector<int>> adj_;
};

}  // namespace cyber::remesh::detail
