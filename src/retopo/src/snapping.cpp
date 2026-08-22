#include "cyber/retopo/snapping.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace cyber::retopo {

namespace {

constexpr std::uint32_t kVertexLeafSize = 8;
// Depth of the fixed traversal stack. The build is a median split, so the depth
// is logarithmic in the vertex count and cannot approach this.
constexpr std::size_t kVertexStack = 64;

float distanceSquaredToBox(Vec3 p, Vec3 lo, Vec3 hi) {
    const float dx = std::fmax(std::fmax(lo.x - p.x, 0.0f), p.x - hi.x);
    const float dy = std::fmax(std::fmax(lo.y - p.y, 0.0f), p.y - hi.y);
    const float dz = std::fmax(std::fmax(lo.z - p.z, 0.0f), p.z - hi.z);
    return dx * dx + dy * dy + dz * dz;
}

float axisOf(Vec3 v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

}  // namespace

SurfaceSnapper::SurfaceSnapper(const Mesh& target) : m_bvh(target) {
    m_vertices.reserve(target.vertexCount());
    for (Index i = 0; i < target.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!target.isAlive(v)) {
            continue;
        }
        m_vertices.push_back({v, target.position(v)});
    }
    if (m_vertices.empty()) {
        return;
    }
    m_vertexNodes.reserve(m_vertices.size() / kVertexLeafSize * 2 + 2);
    m_vertexNodes.emplace_back();
    buildVertexNode(0, 0, static_cast<std::uint32_t>(m_vertices.size()));
}

void SurfaceSnapper::buildVertexNode(std::uint32_t node, std::uint32_t begin, std::uint32_t end) {
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
    Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max()};
    for (std::uint32_t i = begin; i < end; ++i) {
        lo = min(lo, m_vertices[i].position);
        hi = max(hi, m_vertices[i].position);
    }
    m_vertexNodes[node].boundsMin = lo;
    m_vertexNodes[node].boundsMax = hi;

    if (end - begin <= kVertexLeafSize) {
        m_vertexNodes[node].first = begin;
        m_vertexNodes[node].count = end - begin;
        return;
    }

    const Vec3 extent = hi - lo;
    int axis = 0;
    if (extent.y > extent.x) {
        axis = 1;
    }
    if (extent.z > axisOf(extent, axis)) {
        axis = 2;
    }
    const std::uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(m_vertices.begin() + begin, m_vertices.begin() + mid, m_vertices.begin() + end,
                     [axis](const VertexRecord& x, const VertexRecord& y) {
                         const float cx = axisOf(x.position, axis);
                         const float cy = axisOf(y.position, axis);
                         if (cx != cy) {
                             return cx < cy;
                         }
                         return x.id.value < y.id.value;
                     });

    const auto left = static_cast<std::uint32_t>(m_vertexNodes.size());
    m_vertexNodes[node].first = left;
    m_vertexNodes[node].count = 0;
    m_vertexNodes.emplace_back();
    m_vertexNodes.emplace_back();
    buildVertexNode(left, begin, mid);
    buildVertexNode(left + 1, mid, end);
}

SurfaceHit SurfaceSnapper::snapToSurface(Vec3 query) const {
    const Bvh::ClosestHit hit = m_bvh.closestPoint(query);
    return {hit.point, hit.face, hit.distanceSquared};
}

std::optional<VertexHit> SurfaceSnapper::snapToVertex(Vec3 query, float radius) const {
    const float radiusSquared = radius * radius;
    std::optional<VertexHit> best;
    if (m_vertexNodes.empty()) {
        return best;
    }

    // A candidate replaces the incumbent when it is strictly closer, or exactly
    // as close and lower-id — which is what a scan in id order picks.
    const auto consider = [&best, query, radiusSquared](const VertexRecord& rec) {
        const float d2 = lengthSquared(rec.position - query);
        if (d2 > radiusSquared) {
            return;
        }
        if (!best || d2 < best->distanceSquared ||
            (d2 == best->distanceSquared && rec.id.value < best->vertex.value)) {
            best = VertexHit{rec.id, rec.position, d2};
        }
    };
    // A box exactly at the incumbent's distance is NOT pruned: it may hold an
    // equidistant vertex with a lower id, which outranks the incumbent.
    const auto pruned = [&best, radiusSquared](float boxDistanceSquared) {
        return boxDistanceSquared > radiusSquared ||
               (best && boxDistanceSquared > best->distanceSquared);
    };

    struct Entry {
        std::uint32_t node;
        float distanceSquared;
    };
    std::array<Entry, kVertexStack> stack{};
    std::size_t top = 0;
    stack[top++] = {
        0, distanceSquaredToBox(query, m_vertexNodes[0].boundsMin, m_vertexNodes[0].boundsMax)};
    while (top > 0) {
        const Entry entry = stack[--top];
        if (pruned(entry.distanceSquared)) {
            continue;
        }
        const VertexNode& node = m_vertexNodes[entry.node];
        if (node.count > 0) {
            for (std::uint32_t i = 0; i < node.count; ++i) {
                consider(m_vertices[node.first + i]);
            }
            continue;
        }
        Entry nearChild{node.first, distanceSquaredToBox(query, m_vertexNodes[node.first].boundsMin,
                                                         m_vertexNodes[node.first].boundsMax)};
        Entry farChild{node.first + 1,
                       distanceSquaredToBox(query, m_vertexNodes[node.first + 1].boundsMin,
                                            m_vertexNodes[node.first + 1].boundsMax)};
        if (farChild.distanceSquared < nearChild.distanceSquared) {
            std::swap(nearChild, farChild);
        }
        if (top + 2 <= stack.size()) {
            if (!pruned(farChild.distanceSquared)) {
                stack[top++] = farChild;
            }
            if (!pruned(nearChild.distanceSquared)) {
                stack[top++] = nearChild;
            }
        }
    }
    return best;
}

}  // namespace cyber::retopo
