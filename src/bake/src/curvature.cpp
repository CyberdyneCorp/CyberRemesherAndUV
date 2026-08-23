#include "cyber/bake/curvature.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace cyber::bake {

namespace {

// Cotangent-weighted mean-curvature-normal accumulator plus the mixed Voronoi
// area that normalizes it (Meyer et al. 2003).
struct CurvatureAccum {
    std::vector<Vec3> laplacian;  // sum_j (cot a + cot b) * (p_v - p_j)
    std::vector<float> area;      // A_mixed per vertex
};

// Per-vertex normals, area-weighted by Newell face normals (the same
// convention bake.cpp uses to orient its projection rays).
std::vector<Vec3> vertexNormals(const Mesh& mesh) {
    std::vector<Vec3> normals(mesh.vertexCapacity(), Vec3{});
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const Vec3 fn = mesh.faceNormal(f);
        for (const VertexId v : mesh.faceVertices(f)) {
            normals[v.value] += fn;
        }
    }
    for (Vec3& n : normals) {
        n = normalized(n);
    }
    return normals;
}

// Cotangent of the angle at `apex` in the triangle (apex, u, w).
float cotangentAt(Vec3 apex, Vec3 u, Vec3 w) {
    const Vec3 e0 = u - apex;
    const Vec3 e1 = w - apex;
    const float sin2 = length(cross(e0, e1));
    if (sin2 < 1e-20f) {
        return 0.0f;  // degenerate corner contributes no weight
    }
    return dot(e0, e1) / sin2;
}

// Mixed Voronoi area of triangle (p0,p1,p2) apportioned to its three corners.
// Voronoi-exact when the triangle is acute; the obtuse case falls back to the
// halved/quartered area, which keeps every share positive (a raw Voronoi area
// goes negative on obtuse corners and would flip the curvature sign there).
std::array<float, 3> mixedAreas(const std::array<Vec3, 3>& p, const std::array<float, 3>& cot) {
    const float twiceArea = length(cross(p[1] - p[0], p[2] - p[0]));
    const float area = 0.5f * twiceArea;
    if (area < 1e-20f) {
        return {0.0f, 0.0f, 0.0f};
    }
    int obtuseCorner = -1;
    for (int k = 0; k < 3; ++k) {
        const std::size_t i = static_cast<std::size_t>(k);
        if (dot(p[(i + 1) % 3] - p[i], p[(i + 2) % 3] - p[i]) < 0.0f) {
            obtuseCorner = k;
        }
    }
    if (obtuseCorner >= 0) {
        std::array<float, 3> out{area * 0.25f, area * 0.25f, area * 0.25f};
        out[static_cast<std::size_t>(obtuseCorner)] = area * 0.5f;
        return out;
    }
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        // Corner i's Voronoi cell is bounded by the two incident edges,
        // weighted by the cotangents of the angles opposite each of them.
        const float lenNext = lengthSquared(p[(i + 1) % 3] - p[i]);
        const float lenPrev = lengthSquared(p[(i + 2) % 3] - p[i]);
        out[i] = (lenNext * cot[(i + 2) % 3] + lenPrev * cot[(i + 1) % 3]) * 0.125f;
    }
    return out;
}

void accumulateTriangle(const std::array<VertexId, 3>& v, const std::array<Vec3, 3>& p,
                        CurvatureAccum& acc) {
    std::array<float, 3> cot{};
    for (std::size_t i = 0; i < 3; ++i) {
        cot[i] = cotangentAt(p[i], p[(i + 1) % 3], p[(i + 2) % 3]);
    }
    // The angle at corner i weights the OPPOSITE edge (i+1, i+2).
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t a = (i + 1) % 3;
        const std::size_t b = (i + 2) % 3;
        const Vec3 delta = (p[a] - p[b]) * cot[i];
        acc.laplacian[v[a].value] += delta;
        acc.laplacian[v[b].value] += delta * -1.0f;
    }
    const std::array<float, 3> areas = mixedAreas(p, cot);
    for (std::size_t i = 0; i < 3; ++i) {
        acc.area[v[i].value] += areas[i];
    }
}

CurvatureAccum accumulate(const Mesh& mesh) {
    CurvatureAccum acc;
    acc.laplacian.assign(mesh.vertexCapacity(), Vec3{});
    acc.area.assign(mesh.vertexCapacity(), 0.0f);
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        // Fan-triangulate on the corners, matching the BVH's triangulation.
        const std::vector<VertexId> verts = mesh.faceVertices(f);
        for (std::size_t i = 2; i < verts.size(); ++i) {
            const std::array<VertexId, 3> tri{verts[0], verts[i - 1], verts[i]};
            const std::array<Vec3, 3> p{mesh.position(tri[0]), mesh.position(tri[1]),
                                        mesh.position(tri[2])};
            accumulateTriangle(tri, p, acc);
        }
    }
    return acc;
}

std::vector<bool> boundaryVertices(const Mesh& mesh) {
    std::vector<bool> onBoundary(mesh.vertexCapacity(), false);
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || !mesh.isBoundaryEdge(e)) {
            continue;
        }
        const auto [v0, v1] = mesh.edgeVertices(e);
        onBoundary[v0.value] = true;
        onBoundary[v1.value] = true;
    }
    return onBoundary;
}

// Replaces the (meaningless) boundary-vertex values with the mean of their
// interior neighbours. Reads `curvature` for interior values only, so the
// result does not depend on iteration order.
void smoothBoundary(const Mesh& mesh, const std::vector<bool>& onBoundary,
                    std::vector<float>& curvature) {
    const std::vector<float> interior = curvature;
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!mesh.isAlive(v) || !onBoundary[vi]) {
            continue;
        }
        float sum = 0.0f;
        int count = 0;
        for (const EdgeId e : mesh.vertexEdges(v)) {
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId other = (a.value == vi) ? b : a;
            if (!onBoundary[other.value]) {
                sum += interior[other.value];
                ++count;
            }
        }
        curvature[vi] = (count > 0) ? sum / static_cast<float>(count) : 0.0f;
    }
}

// Smallest |H| whose cumulative weight, over values sorted ascending, reaches
// `p` of the total. With equal weights this is the plain rank percentile.
float weightedPercentile(std::vector<std::pair<float, float>>& samples, float p) {
    if (samples.empty()) {
        return 0.0f;
    }
    std::sort(samples.begin(), samples.end());
    double total = 0.0;
    for (const auto& [value, weight] : samples) {
        total += weight;
    }
    const double target = static_cast<double>(std::clamp(p, 0.0f, 1.0f)) * total;
    double cumulative = 0.0;
    for (const auto& [value, weight] : samples) {
        cumulative += weight;
        if (cumulative >= target) {
            return value;
        }
    }
    return samples.back().first;
}

}  // namespace

std::vector<float> vertexMeanCurvature(const Mesh& mesh, std::vector<float>* vertexAreas) {
    std::vector<float> curvature(mesh.vertexCapacity(), 0.0f);
    if (vertexAreas != nullptr) {
        vertexAreas->assign(mesh.vertexCapacity(), 0.0f);
    }
    if (mesh.faceCount() == 0) {
        return curvature;
    }
    const CurvatureAccum acc = accumulate(mesh);
    const std::vector<Vec3> normals = vertexNormals(mesh);

    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        if (!mesh.isAlive(VertexId{vi}) || acc.area[vi] < 1e-20f) {
            continue;
        }
        // K = laplacian / (2A) and K = 2*H*n, so H = dot(laplacian, n) / (4A).
        curvature[vi] = dot(acc.laplacian[vi], normals[vi]) / (4.0f * acc.area[vi]);
    }

    const std::vector<bool> onBoundary = boundaryVertices(mesh);
    smoothBoundary(mesh, onBoundary, curvature);
    if (vertexAreas != nullptr) {
        for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
            (*vertexAreas)[vi] = mesh.isAlive(VertexId{vi}) ? acc.area[vi] : 0.0f;
        }
    }
    return curvature;
}

float curvatureScale(const std::vector<float>& curvature, float percentile) {
    return curvatureScale(curvature, {}, percentile);
}

float curvatureScale(const std::vector<float>& curvature, const std::vector<float>& weights,
                     float percentile) {
    const bool weighted = weights.size() == curvature.size();
    std::vector<std::pair<float, float>> samples;
    samples.reserve(curvature.size());
    for (std::size_t i = 0; i < curvature.size(); ++i) {
        const float w = weighted ? weights[i] : 1.0f;
        if (curvature[i] != 0.0f && w > 0.0f) {
            samples.emplace_back(std::fabs(curvature[i]), w);
        }
    }
    return weightedPercentile(samples, percentile);
}

}  // namespace cyber::bake
