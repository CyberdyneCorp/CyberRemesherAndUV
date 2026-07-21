#include "cyber/quadrangulate/mcf_layout.hpp"

#include <vector>

#include "mcf_detail.hpp"

// M5a of docs/mcf-integer-layout-plan.md: the extraction vertex-collapse — the opening
// of QuadriFlow's extraction (ComputeSharpO / AdvancedExtractQuad). Vertices joined by
// a zero edge_diff share a lattice cell and merge into one output quad vertex. Pure
// integer math — no SciPP. Face tracing (M5b) builds the quads over these vertices.
namespace cyber::remesh {

namespace {

// Union-find with path compression + union by size.
struct DisjointSet {
    std::vector<int> parent, size;
    explicit DisjointSet(int n) : parent(mcf::uz(n)), size(mcf::uz(n), 1) {
        for (int i = 0; i < n; ++i) {
            parent[mcf::uz(i)] = i;
        }
    }
    int find(int x) {
        while (parent[mcf::uz(x)] != x) {
            parent[mcf::uz(x)] = parent[mcf::uz(parent[mcf::uz(x)])];
            x = parent[mcf::uz(x)];
        }
        return x;
    }
    void merge(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (size[mcf::uz(a)] < size[mcf::uz(b)]) {
            std::swap(a, b);
        }
        parent[mcf::uz(b)] = a;
        size[mcf::uz(a)] += size[mcf::uz(b)];
    }
};

}  // namespace

McfCollapse buildMcfCollapse(const Mesh& mesh, const PositionField& field, const McfEdgeInfo& info,
                             const std::vector<Vec2i>& edgeDiff) {
    using mcf::uz;
    McfCollapse out;
    if (!info.valid || edgeDiff.size() != info.edgeValues.size()) {
        return out;
    }
    const int nV = static_cast<int>(mesh.vertexCapacity());
    DisjointSet ds(nV);
    for (std::size_t e = 0; e < edgeDiff.size(); ++e) {
        if (edgeDiff[e].x == 0 && edgeDiff[e].y == 0) {
            const auto [a, b] = info.edgeValues[e];
            ds.merge(static_cast<int>(a), static_cast<int>(b));
        }
    }

    // Only vertices used by a live triangle become output vertices; compact the roots.
    std::vector<char> used(uz(nV), 0);
    for (const auto& fv : info.faceVerts) {
        for (const Index v : fv) {
            used[v] = 1;
        }
    }
    out.component.assign(uz(nV), -1);
    std::vector<int> rootToId(uz(nV), -1);
    std::vector<Vec3> accum;
    std::vector<int> count;
    for (int v = 0; v < nV; ++v) {
        if (!used[uz(v)]) {
            continue;
        }
        const int r = ds.find(v);
        if (rootToId[uz(r)] == -1) {
            rootToId[uz(r)] = out.numVertices++;
            accum.push_back(Vec3{0, 0, 0});
            count.push_back(0);
        }
        const int id = rootToId[uz(r)];
        out.component[uz(v)] = id;
        const Vec3 o = field.o[uz(v)];
        accum[uz(id)] = Vec3{accum[uz(id)].x + o.x, accum[uz(id)].y + o.y, accum[uz(id)].z + o.z};
        count[uz(id)] += 1;
    }
    out.position.resize(uz(out.numVertices));
    for (int i = 0; i < out.numVertices; ++i) {
        const float inv = count[uz(i)] > 0 ? 1.0f / static_cast<float>(count[uz(i)]) : 1.0f;
        out.position[uz(i)] = Vec3{accum[uz(i)].x * inv, accum[uz(i)].y * inv, accum[uz(i)].z * inv};
    }

    out.valid = true;
    return out;
}

}  // namespace cyber::remesh
