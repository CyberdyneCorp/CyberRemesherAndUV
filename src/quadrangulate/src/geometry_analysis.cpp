#include "cyber/quadrangulate/geometry_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace cyber::remesh {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInf = std::numeric_limits<float>::infinity();

float safeGet(const std::vector<float>& field, VertexId v, float fallback) {
    return v.valid() && v.value < field.size() ? field[v.value] : fallback;
}

// Unit normal per alive face; zero for degenerate ones so they drop out of the
// deviation maximum instead of poisoning it with a NaN direction.
std::vector<Vec3> faceNormals(const Mesh& mesh) {
    std::vector<Vec3> out(mesh.faceCapacity(), Vec3{});
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        const Vec3 n = mesh.faceNormal(face);
        const float len = length(n);
        if (len > 1e-20f) {
            out[f] = n / len;
        }
    }
    return out;
}

// Area-weighted vertex normals, for the thickness probe's inward direction.
std::vector<Vec3> vertexNormals(const Mesh& mesh, const std::vector<Vec3>& fn) {
    std::vector<Vec3> out(mesh.vertexCapacity(), Vec3{});
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        Vec3 acc{};
        for (const FaceId f : mesh.vertexFaces(vid)) {
            acc += fn[f.value];
        }
        const float len = length(acc);
        out[v] = len > 1e-20f ? acc / len : Vec3{};
    }
    return out;
}

// Widest angle between any two face normals around a vertex, over pi.
std::vector<float> curvatureField(const Mesh& mesh, const std::vector<Vec3>& fn) {
    std::vector<float> out(mesh.vertexCapacity(), 0.0f);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        const std::vector<FaceId> faces = mesh.vertexFaces(vid);
        float worst = 0.0f;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            const Vec3 a = fn[faces[i].value];
            if (length(a) < 0.5f) {
                continue;  // degenerate face
            }
            for (std::size_t j = i + 1; j < faces.size(); ++j) {
                const Vec3 b = fn[faces[j].value];
                if (length(b) < 0.5f) {
                    continue;
                }
                worst = std::max(worst, std::acos(std::clamp(dot(a, b), -1.0f, 1.0f)));
            }
        }
        out[v] = std::clamp(worst / kPi, 0.0f, 1.0f);
    }
    return out;
}

// Multi-source shortest path over mesh edges from `seeds`, converted to an
// influence that is 1 at a seed and 0 at `radius`. Ties break on vertex id, so
// the field is identical run to run.
std::vector<float> proximityField(const Mesh& mesh, const std::vector<VertexId>& seeds,
                                  float radius) {
    std::vector<float> influence(mesh.vertexCapacity(), 0.0f);
    if (seeds.empty() || radius <= 0.0f) {
        return influence;
    }
    std::vector<float> dist(mesh.vertexCapacity(), kInf);
    using Entry = std::pair<float, Index>;  // (distance, vertex) — id breaks ties
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (const VertexId s : seeds) {
        if (s.valid() && s.value < dist.size() && dist[s.value] != 0.0f) {
            dist[s.value] = 0.0f;
            queue.push({0.0f, s.value});
        }
    }
    while (!queue.empty()) {
        const auto [d, v] = queue.top();
        queue.pop();
        if (d > dist[v]) {
            continue;  // stale entry
        }
        for (const EdgeId e : mesh.vertexEdges(VertexId{v})) {
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId other = a.value == v ? b : a;
            if (!other.valid() || other.value >= dist.size()) {
                continue;
            }
            const float step = length(mesh.position(other) - mesh.position(VertexId{v}));
            const float nd = d + step;
            if (nd < dist[other.value] && nd < radius) {
                dist[other.value] = nd;
                queue.push({nd, other.value});
            }
        }
    }
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (dist[v] < radius) {
            influence[v] = 1.0f - dist[v] / radius;
        }
    }
    return influence;
}

std::vector<VertexId> verticesOnEdgesWhere(const Mesh& mesh, bool wantFeature) {
    std::vector<VertexId> seeds;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e) {
        const EdgeId edge{e};
        if (!mesh.isAlive(edge)) {
            continue;
        }
        const bool hit = wantFeature ? mesh.isFeatureEdge(edge) : mesh.isBoundaryEdge(edge);
        if (!hit) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(edge);
        seeds.push_back(a);
        seeds.push_back(b);
    }
    return seeds;
}

// Nearest opposing-surface hit along `dir`, ignoring the immediate
// neighbourhood: the probe starts a short way in so it cannot re-hit the
// triangles that share the origin vertex.
float opposingHit(const Bvh& bvh, Vec3 origin, Vec3 dir, float epsilon) {
    const std::optional<Bvh::RayHit> hit = bvh.raycast(origin + dir * epsilon, dir);
    return hit.has_value() ? hit->t + epsilon : kInf;
}

// Thickness by an inward ray fan. A single -n ray misses on curved tubes, so a
// small fan around it is cast and the nearest hit wins.
std::vector<float> thicknessField(const Mesh& mesh, const Bvh& bvh,
                                  const std::vector<Vec3>& normals,
                                  const GeometryAnalysisOptions& options, float epsilon) {
    std::vector<float> out(mesh.vertexCapacity(), kInf);
    const int samples = std::max(1, options.thicknessFanSamples);
    const float half = options.thicknessFanDegrees * kPi / 180.0f;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        const Vec3 n = normals[v];
        if (length(n) < 0.5f) {
            continue;
        }
        const Vec3 p = mesh.position(vid);
        const Vec3 inward = n * -1.0f;
        // An orthonormal basis around the inward direction, for the fan.
        const Vec3 seed = std::abs(inward.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 t1 = normalized(cross(inward, seed));
        const Vec3 t2 = cross(inward, t1);
        float best = opposingHit(bvh, p, inward, epsilon);
        for (int k = 0; k < samples; ++k) {
            const float phi = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(samples);
            const Vec3 offset =
                t1 * (std::cos(phi) * std::sin(half)) + t2 * (std::sin(phi) * std::sin(half));
            const Vec3 dir = normalized(inward * std::cos(half) + offset);
            best = std::min(best, opposingHit(bvh, p, dir, epsilon));
        }
        out[v] = best;
    }
    return out;
}

float bboxDiagonal(const Mesh& mesh) {
    Vec3 lo{kInf, kInf, kInf};
    Vec3 hi{-kInf, -kInf, -kInf};
    bool any = false;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        const Vec3 p = mesh.position(vid);
        lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        any = true;
    }
    return any ? length(hi - lo) : 0.0f;
}

}  // namespace

float GeometryAnalysis::curvatureAt(VertexId v) const {
    return safeGet(normalizedCurvature, v, 0.0f);
}
float GeometryAnalysis::featureAt(VertexId v) const { return safeGet(featureInfluence, v, 0.0f); }
float GeometryAnalysis::boundaryAt(VertexId v) const { return safeGet(boundaryInfluence, v, 0.0f); }
float GeometryAnalysis::thinRiskAt(VertexId v) const { return safeGet(thinFeatureRisk, v, 0.0f); }

GeometryAnalysis analyzeGeometry(const Mesh& mesh, float targetEdgeLength, const Bvh* bvh,
                                 const GeometryAnalysisOptions& options) {
    GeometryAnalysis out;
    if (mesh.vertexCapacity() == 0) {
        return out;
    }
    const float diag = bboxDiagonal(mesh);
    const float radius = options.influenceRadius > 0.0f ? options.influenceRadius : diag * 0.05f;

    const std::vector<Vec3> fn = faceNormals(mesh);
    out.normalizedCurvature = curvatureField(mesh, fn);
    out.featureInfluence = proximityField(mesh, verticesOnEdgesWhere(mesh, true), radius);
    out.boundaryInfluence = proximityField(mesh, verticesOnEdgesWhere(mesh, false), radius);

    out.thickness.assign(mesh.vertexCapacity(), kInf);
    out.thinFeatureRisk.assign(mesh.vertexCapacity(), 0.0f);
    if (bvh != nullptr && !bvh->empty() && targetEdgeLength > 0.0f) {
        // Step the probe off the surface by well under one target edge so it
        // clears the origin's own triangles without skipping a genuinely thin
        // feature.
        const float epsilon = targetEdgeLength * 1e-2f;
        out.thickness = thicknessField(mesh, *bvh, vertexNormals(mesh, fn), options, epsilon);
        const float thin = std::max(1.0f, options.thinFeatureFactor) * targetEdgeLength;
        for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
            const float t = out.thickness[v];
            if (std::isfinite(t) && t < thin) {
                out.thinFeatureRisk[v] = std::clamp(1.0f - t / thin, 0.0f, 1.0f);
            }
        }
    }
    out.valid = true;
    return out;
}

}  // namespace cyber::remesh
