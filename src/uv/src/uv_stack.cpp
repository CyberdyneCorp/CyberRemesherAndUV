#include "cyber/uv/uv_stack.hpp"

#include <cmath>
#include <limits>
#include <unordered_set>

#include "cyber/uv/common.hpp"

namespace cyber::uv {
namespace {

// Reflection of `p` across the plane through `point` with unit-ish `normal`.
[[nodiscard]] Vec3 reflect(Vec3 p, Vec3 point, Vec3 normal) {
    const float n2 = lengthSquared(normal);
    if (n2 <= 0.0f) {
        return p;
    }
    const float d = dot(p - point, normal) / n2;
    return p - normal * (2.0f * d);
}

// Centroid of an island's face centroids — a cheap, order-independent signature for matching.
[[nodiscard]] Vec3 islandCentroid(const Mesh& mesh, std::span<const FaceId> island) {
    Vec3 sum{};
    std::size_t count = 0;
    for (const FaceId face : island) {
        sum += mesh.faceCentroid(face);
        ++count;
    }
    if (count == 0) {
        return sum;
    }
    return sum * (1.0f / static_cast<float>(count));
}

// Largest distance from an island's centroid to any of its face centroids, used to scale the
// match tolerance: a fixed world tolerance would be far too tight on a large shell and far too
// loose on a small one.
[[nodiscard]] float islandRadius(const Mesh& mesh, std::span<const FaceId> island, Vec3 centroid) {
    float worst = 0.0f;
    for (const FaceId face : island) {
        worst = std::fmax(worst, length(mesh.faceCentroid(face) - centroid));
    }
    return worst;
}

}  // namespace

std::vector<MirrorIslandPair> findMirrorIslandPairs(const Mesh& mesh,
                                                   std::span<const std::vector<FaceId>> islands,
                                                   Vec3 planePoint, Vec3 planeNormal,
                                                   float tolerance) {
    std::vector<MirrorIslandPair> pairs;
    std::unordered_set<std::size_t> taken;

    for (std::size_t i = 0; i < islands.size(); ++i) {
        if (taken.count(i) != 0 || islands[i].empty()) {
            continue;
        }
        const Vec3 centroidI = islandCentroid(mesh, islands[i]);
        const Vec3 mirrored = reflect(centroidI, planePoint, planeNormal);

        // An island lying ON the plane is its own mirror. Stacking it onto itself would be a
        // no-op reported as progress, so it is skipped rather than self-paired.
        if (length(mirrored - centroidI) <= tolerance) {
            continue;
        }

        std::size_t best = islands.size();
        float bestDistance = std::numeric_limits<float>::max();
        for (std::size_t j = i + 1; j < islands.size(); ++j) {
            if (taken.count(j) != 0 || islands[j].size() != islands[i].size()) {
                // Same face count is a necessary condition for a corner-for-corner
                // correspondence, so it is cheap to reject on first.
                continue;
            }
            const float distance = length(islandCentroid(mesh, islands[j]) - mirrored);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = j;
            }
        }
        if (best == islands.size()) {
            continue;
        }
        // Scaled by the island's own size, so the same tolerance means the same thing on a
        // fingertip and on a torso.
        const float scale = std::fmax(islandRadius(mesh, islands[i], centroidI), 1e-6f);
        if (bestDistance <= tolerance * scale) {
            pairs.push_back({i, best});
            taken.insert(i);
            taken.insert(best);
        }
    }
    return pairs;
}

std::size_t stackMirroredIslands(Mesh& mesh, std::span<const std::vector<FaceId>> islands,
                                 std::span<const MirrorIslandPair> pairs, Vec3 planePoint,
                                 Vec3 planeNormal, float tolerance) {
    std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return 0;
    }
    std::size_t stacked = 0;

    for (const MirrorIslandPair& pair : pairs) {
        if (pair.primary >= islands.size() || pair.mirror >= islands.size()) {
            continue;
        }
        const std::vector<FaceId>& source = islands[pair.primary];
        const std::vector<FaceId>& target = islands[pair.mirror];

        // Corners of the source, with their world positions, so the target's mirrored corners can
        // be matched against them by GEOMETRY. Matching by corner index instead — which is what
        // `cloneIslandUv` does — maps the wrong corners across a mirror and produces a scrambled
        // shell that still looks plausible in the 2D view.
        std::vector<Vec3> sourcePositions;
        std::vector<Vec2> sourceUvs;
        for (const FaceId face : source) {
            for (const LoopId loop : mesh.faceLoops(face)) {
                sourcePositions.push_back(mesh.position(mesh.loopVertex(loop)));
                sourceUvs.push_back((*uv)[static_cast<std::size_t>(loop.value)]);
            }
        }
        if (sourcePositions.empty()) {
            continue;
        }

        const Vec3 centroid = islandCentroid(mesh, target);
        const float scale = std::fmax(islandRadius(mesh, target, centroid), 1e-6f);
        const float limit = tolerance * scale;

        // Resolved into a staging buffer FIRST and only written if every corner matched, so a
        // partial correspondence cannot leave half a shell stacked and half where it was.
        std::vector<std::pair<std::size_t, Vec2>> writes;
        bool complete = true;
        for (const FaceId face : target) {
            for (const LoopId loop : mesh.faceLoops(face)) {
                const Vec3 want = reflect(mesh.position(mesh.loopVertex(loop)), planePoint,
                                          planeNormal);
                std::size_t best = sourcePositions.size();
                float bestDistance = std::numeric_limits<float>::max();
                for (std::size_t k = 0; k < sourcePositions.size(); ++k) {
                    const float distance = length(sourcePositions[k] - want);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        best = k;
                    }
                }
                if (best == sourcePositions.size() || bestDistance > limit) {
                    complete = false;
                    break;
                }
                writes.emplace_back(static_cast<std::size_t>(loop.value), sourceUvs[best]);
            }
            if (!complete) {
                break;
            }
        }
        if (!complete) {
            continue;
        }
        for (const auto& [loop, value] : writes) {
            (*uv)[loop] = value;
        }
        ++stacked;
    }
    return stacked;
}

}  // namespace cyber::uv
