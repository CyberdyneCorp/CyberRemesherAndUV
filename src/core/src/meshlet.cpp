#include "cyber/core/meshlet.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace cyber::remesh {

namespace {

Vec3 positionAt(std::span<const float> positions, std::uint32_t vertex) {
    const std::size_t base = static_cast<std::size_t>(vertex) * 3;
    return Vec3{positions[base], positions[base + 1], positions[base + 2]};
}

/// Triangle normal, or a zero vector for a degenerate triangle.
Vec3 triangleNormal(std::span<const float> positions, std::uint32_t a, std::uint32_t b,
                    std::uint32_t c) {
    const Vec3 pa = positionAt(positions, a);
    const Vec3 n = cross(positionAt(positions, b) - pa, positionAt(positions, c) - pa);
    const float len = length(n);
    return len > 0.0f ? n * (1.0f / len) : Vec3{};
}

/// Finalises a cluster's bounding sphere and normal cone.
///
/// The sphere is centre-of-extremes plus max radius: not the minimal enclosing
/// sphere, but conservative (never too small), which is the only property a
/// culling test may rely on. A too-LARGE sphere costs a missed cull; a too-small
/// one drops visible geometry.
void finaliseBounds(Meshlet& meshlet, std::span<const float> positions,
                    const MeshletSet& set) {
    Vec3 lo{}, hi{};
    bool first = true;
    Vec3 normalSum{};
    std::size_t normalCount = 0;

    for (std::uint32_t i = 0; i < meshlet.vertexCount; ++i) {
        const Vec3 p = positionAt(positions, set.vertices[meshlet.vertexOffset + i]);
        if (first) {
            lo = hi = p;
            first = false;
        } else {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    meshlet.center = (lo + hi) * 0.5f;
    float radius = 0.0f;
    for (std::uint32_t i = 0; i < meshlet.vertexCount; ++i) {
        const Vec3 p = positionAt(positions, set.vertices[meshlet.vertexOffset + i]);
        radius = std::max(radius, length(p - meshlet.center));
    }
    meshlet.radius = radius;

    for (std::uint32_t t = 0; t < meshlet.triangleCount; ++t) {
        const std::size_t base = (static_cast<std::size_t>(meshlet.triangleOffset) + t) * 3;
        const std::uint32_t a = set.vertices[meshlet.vertexOffset + set.indices[base]];
        const std::uint32_t b = set.vertices[meshlet.vertexOffset + set.indices[base + 1]];
        const std::uint32_t c = set.vertices[meshlet.vertexOffset + set.indices[base + 2]];
        const Vec3 n = triangleNormal(positions, a, b, c);
        if (length(n) > 0.0f) {
            normalSum += n;
            ++normalCount;
        }
    }

    const float sumLength = length(normalSum);
    if (normalCount == 0 || sumLength <= 0.0f) {
        // No usable normals: must never be backface-culled.
        meshlet.coneAxis = Vec3{0.0f, 0.0f, 1.0f};
        meshlet.coneCutoff = 0.0f;
        return;
    }
    meshlet.coneAxis = normalSum * (1.0f / sumLength);

    float cutoff = 1.0f;
    for (std::uint32_t t = 0; t < meshlet.triangleCount; ++t) {
        const std::size_t base = (static_cast<std::size_t>(meshlet.triangleOffset) + t) * 3;
        const std::uint32_t a = set.vertices[meshlet.vertexOffset + set.indices[base]];
        const std::uint32_t b = set.vertices[meshlet.vertexOffset + set.indices[base + 1]];
        const std::uint32_t c = set.vertices[meshlet.vertexOffset + set.indices[base + 2]];
        const Vec3 n = triangleNormal(positions, a, b, c);
        if (length(n) > 0.0f) {
            cutoff = std::min(cutoff, dot(meshlet.coneAxis, n));
        }
    }
    // Spanning more than a hemisphere means no view direction can be proven to
    // see only back faces, so the cone is unusable: fail safe.
    meshlet.coneCutoff = cutoff > 0.0f ? cutoff : 0.0f;
}

}  // namespace

MeshletSet buildMeshlets(std::span<const float> positions,
                         std::span<const std::uint32_t> indices, MeshletCaps caps) {
    MeshletSet set;
    if (indices.empty() || indices.size() % 3 != 0 || positions.size() % 3 != 0) {
        return set;
    }
    const std::uint32_t vertexCount = static_cast<std::uint32_t>(positions.size() / 3);
    const std::size_t triangleTotal = indices.size() / 3;

    // Caps are clamped, not trusted: 8-bit local indices cap vertices at 256, and
    // a zero cap would loop forever.
    const std::uint32_t maxVertices = std::clamp<std::uint32_t>(caps.maxVertices, 3, 256);
    const std::uint32_t maxTriangles = std::max<std::uint32_t>(caps.maxTriangles, 1);

    // Validate the whole stream first: a malformed index must not yield a
    // partially valid set the renderer would happily draw.
    for (const std::uint32_t index : indices) {
        if (index >= vertexCount) {
            return set;
        }
    }

    // Vertex -> triangles, for adjacency without building a half-edge structure.
    std::vector<std::vector<std::uint32_t>> vertexTriangles(vertexCount);
    std::vector<char> degenerate(triangleTotal, 0);
    for (std::size_t t = 0; t < triangleTotal; ++t) {
        const std::uint32_t a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        if (a == b || b == c || a == c) {
            degenerate[t] = 1;  // dropped: it has no area and no usable normal
            continue;
        }
        vertexTriangles[a].push_back(static_cast<std::uint32_t>(t));
        vertexTriangles[b].push_back(static_cast<std::uint32_t>(t));
        vertexTriangles[c].push_back(static_cast<std::uint32_t>(t));
    }

    std::vector<char> assigned(triangleTotal, 0);
    std::size_t nextSeed = 0;

    while (true) {
        while (nextSeed < triangleTotal && (assigned[nextSeed] || degenerate[nextSeed])) {
            ++nextSeed;
        }
        if (nextSeed >= triangleTotal) {
            break;
        }

        Meshlet meshlet;
        meshlet.vertexOffset = static_cast<std::uint32_t>(set.vertices.size());
        meshlet.triangleOffset = static_cast<std::uint32_t>(set.indices.size() / 3);

        // Local vertex list for this cluster; `local` maps global -> local index.
        std::unordered_map<std::uint32_t, std::uint8_t> local;
        local.reserve(maxVertices * 2);

        const auto addTriangle = [&](std::uint32_t triangle) {
            const std::uint32_t corners[3] = {indices[triangle * 3], indices[triangle * 3 + 1],
                                              indices[triangle * 3 + 2]};
            for (const std::uint32_t corner : corners) {
                const auto [it, inserted] = local.try_emplace(
                    corner, static_cast<std::uint8_t>(local.size()));
                if (inserted) {
                    set.vertices.push_back(corner);
                }
                set.indices.push_back(it->second);
            }
            assigned[triangle] = 1;
            ++meshlet.triangleCount;
        };

        // How many NEW vertices adding this triangle would cost.
        const auto newVertexCost = [&](std::uint32_t triangle) {
            std::uint32_t cost = 0;
            std::uint32_t seen[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
            for (int i = 0; i < 3; ++i) {
                const std::uint32_t corner = indices[triangle * 3 + static_cast<std::size_t>(i)];
                if (local.count(corner) != 0) {
                    continue;
                }
                if (corner == seen[0] || corner == seen[1] || corner == seen[2]) {
                    continue;  // repeated within the same triangle
                }
                seen[i] = corner;
                ++cost;
            }
            return cost;
        };

        addTriangle(static_cast<std::uint32_t>(nextSeed));

        // Grow: repeatedly take the unassigned neighbour sharing the most
        // vertices with the cluster, tie-broken on the LOWER triangle index so
        // the result is deterministic.
        while (meshlet.triangleCount < maxTriangles) {
            std::uint32_t best = UINT32_MAX;
            std::uint32_t bestShared = 0;
            for (const auto& [globalVertex, unusedLocal] : local) {
                (void)unusedLocal;
                for (const std::uint32_t candidate : vertexTriangles[globalVertex]) {
                    if (assigned[candidate]) {
                        continue;
                    }
                    const std::uint32_t cost = newVertexCost(candidate);
                    if (local.size() + cost > maxVertices) {
                        continue;
                    }
                    const std::uint32_t shared = 3 - cost;
                    if (shared > bestShared || (shared == bestShared && candidate < best)) {
                        bestShared = shared;
                        best = candidate;
                    }
                }
            }
            if (best == UINT32_MAX) {
                break;  // nothing adjacent fits; the cluster is closed
            }
            addTriangle(best);
        }

        meshlet.vertexCount = static_cast<std::uint32_t>(local.size());
        finaliseBounds(meshlet, positions, set);
        set.meshlets.push_back(meshlet);
    }
    return set;
}

}  // namespace cyber::remesh
