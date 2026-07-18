#include "cyber/core/bvh.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace cyber {

Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }

    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return a + ab * (d1 / (d1 - d3));
    }

    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }

    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

namespace {

float distanceSquaredToBox(Vec3 p, Vec3 lo, Vec3 hi) {
    const float dx = std::fmax(std::fmax(lo.x - p.x, 0.0f), p.x - hi.x);
    const float dy = std::fmax(std::fmax(lo.y - p.y, 0.0f), p.y - hi.y);
    const float dz = std::fmax(std::fmax(lo.z - p.z, 0.0f), p.z - hi.z);
    return dx * dx + dy * dy + dz * dz;
}

bool rayIntersectsBox(Vec3 origin, Vec3 invDir, Vec3 lo, Vec3 hi, float maxT) {
    float tmin = 0.0f, tmax = maxT;
    for (const auto& [o, inv, l, h] :
         {std::tuple{origin.x, invDir.x, lo.x, hi.x}, std::tuple{origin.y, invDir.y, lo.y, hi.y},
          std::tuple{origin.z, invDir.z, lo.z, hi.z}}) {
        const float t1 = (l - o) * inv;
        const float t2 = (h - o) * inv;
        tmin = std::fmax(tmin, std::fmin(t1, t2));
        tmax = std::fmin(tmax, std::fmax(t1, t2));
    }
    return tmin <= tmax;
}

// Moller-Trumbore, front and back faces both hit.
std::optional<float> rayTriangle(Vec3 origin, Vec3 dir, Vec3 a, Vec3 b, Vec3 c) {
    constexpr float kEpsilon = 1e-9f;
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 pvec = cross(dir, ac);
    const float det = dot(ab, pvec);
    if (std::fabs(det) < kEpsilon) {
        return std::nullopt;
    }
    const float invDet = 1.0f / det;
    const Vec3 tvec = origin - a;
    const float u = dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) {
        return std::nullopt;
    }
    const Vec3 qvec = cross(tvec, ab);
    const float v = dot(dir, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) {
        return std::nullopt;
    }
    const float t = dot(ac, qvec) * invDet;
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

}  // namespace

Bvh::Bvh(const Mesh& mesh) {
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> verts = mesh.faceVertices(f);
        for (std::size_t i = 2; i < verts.size(); ++i) {
            m_triangles.push_back(
                {mesh.position(verts[0]), mesh.position(verts[i - 1]), mesh.position(verts[i]), f});
        }
    }
    if (m_triangles.empty()) {
        return;
    }
    m_nodes.reserve(m_triangles.size() * 2);
    m_nodes.emplace_back();
    build(0, 0, static_cast<std::uint32_t>(m_triangles.size()));
}

void Bvh::build(std::uint32_t nodeIndex, std::uint32_t begin, std::uint32_t end) {
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
    Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max()};
    for (std::uint32_t i = begin; i < end; ++i) {
        for (const Vec3& p : {m_triangles[i].a, m_triangles[i].b, m_triangles[i].c}) {
            lo = min(lo, p);
            hi = max(hi, p);
        }
    }
    m_nodes[nodeIndex].boundsMin = lo;
    m_nodes[nodeIndex].boundsMax = hi;

    constexpr std::uint32_t kLeafSize = 4;
    if (end - begin <= kLeafSize) {
        m_nodes[nodeIndex].firstTriangle = begin;
        m_nodes[nodeIndex].triangleCount = end - begin;
        return;
    }

    // Median split along the widest axis (deterministic).
    const Vec3 extent = hi - lo;
    int axis = 0;
    if (extent.y > extent.x) {
        axis = 1;
    }
    if (extent.z > (axis == 0 ? extent.x : extent.y)) {
        axis = 2;
    }
    auto centroidAxis = [axis](const Triangle& t) {
        const Vec3 c = (t.a + t.b + t.c) / 3.0f;
        return axis == 0 ? c.x : axis == 1 ? c.y : c.z;
    };
    const std::uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(m_triangles.begin() + begin, m_triangles.begin() + mid,
                     m_triangles.begin() + end, [&](const Triangle& x, const Triangle& y) {
                         const float cx = centroidAxis(x);
                         const float cy = centroidAxis(y);
                         if (cx != cy) {
                             return cx < cy;
                         }
                         return x.face.value < y.face.value;
                     });

    const auto left = static_cast<std::uint32_t>(m_nodes.size());
    m_nodes[nodeIndex].leftChild = left;
    m_nodes.emplace_back();
    m_nodes.emplace_back();
    build(left, begin, mid);
    build(left + 1, mid, end);
}

Bvh::ClosestHit Bvh::closestPoint(Vec3 query) const {
    ClosestHit best;
    best.distanceSquared = std::numeric_limits<float>::max();
    if (m_nodes.empty()) {
        return best;
    }
    std::array<std::uint32_t, 64> stack{};
    std::size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = m_nodes[stack[--top]];
        if (distanceSquaredToBox(query, node.boundsMin, node.boundsMax) >= best.distanceSquared) {
            continue;
        }
        if (node.isLeaf()) {
            for (std::uint32_t i = 0; i < node.triangleCount; ++i) {
                const Triangle& tri = m_triangles[node.firstTriangle + i];
                const Vec3 p = closestPointOnTriangle(query, tri.a, tri.b, tri.c);
                const float d2 = lengthSquared(p - query);
                if (d2 < best.distanceSquared) {
                    best = {p, d2, tri.face};
                }
            }
            continue;
        }
        if (top + 2 <= stack.size()) {
            stack[top++] = node.leftChild;
            stack[top++] = node.leftChild + 1;
        }
    }
    return best;
}

std::optional<Bvh::RayHit> Bvh::raycast(Vec3 origin, Vec3 direction, float maxDistance) const {
    if (m_nodes.empty()) {
        return std::nullopt;
    }
    const Vec3 dir = normalized(direction);
    const Vec3 invDir{1.0f / (dir.x != 0.0f ? dir.x : 1e-30f),
                      1.0f / (dir.y != 0.0f ? dir.y : 1e-30f),
                      1.0f / (dir.z != 0.0f ? dir.z : 1e-30f)};
    std::optional<RayHit> best;
    float bestT = maxDistance;

    std::array<std::uint32_t, 64> stack{};
    std::size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = m_nodes[stack[--top]];
        if (!rayIntersectsBox(origin, invDir, node.boundsMin, node.boundsMax, bestT)) {
            continue;
        }
        if (node.isLeaf()) {
            for (std::uint32_t i = 0; i < node.triangleCount; ++i) {
                const Triangle& tri = m_triangles[node.firstTriangle + i];
                if (const auto t = rayTriangle(origin, dir, tri.a, tri.b, tri.c)) {
                    if (*t < bestT) {
                        bestT = *t;
                        best = RayHit{origin + dir * *t, *t, tri.face};
                    }
                }
            }
            continue;
        }
        if (top + 2 <= stack.size()) {
            stack[top++] = node.leftChild;
            stack[top++] = node.leftChild + 1;
        }
    }
    return best;
}

}  // namespace cyber
