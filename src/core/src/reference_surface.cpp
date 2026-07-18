#include "cyber/core/reference_surface.hpp"

#include <algorithm>
#include <cmath>

namespace cyber::remesh {

namespace {

// Interior angle of face `f` at vertex `v` (used to angle-weight the corner
// normal — Thürmer & Wüthrich, less biased than uniform averaging on
// irregular triangulations).
float cornerAngle(const Mesh& mesh, FaceId f, VertexId v) {
    const std::vector<VertexId> verts = mesh.faceVertices(f);
    const std::size_t n = verts.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (!(verts[i] == v)) {
            continue;
        }
        const Vec3 p = mesh.position(v);
        const Vec3 prev = normalized(mesh.position(verts[(i + n - 1) % n]) - p);
        const Vec3 next = normalized(mesh.position(verts[(i + 1) % n]) - p);
        return std::acos(std::clamp(dot(prev, next), -1.0f, 1.0f));
    }
    return 0.0f;
}

// Crease-aware corner normal: angle-weighted average of the normals of faces
// sharing `v` whose normal is within `cosThreshold` of `faceNormal(f)`. The
// owning face always contributes, so the result never degenerates to zero.
Vec3 smoothedCornerNormal(const Mesh& mesh, VertexId v, Vec3 faceN, float cosThreshold) {
    Vec3 sum{};
    for (const FaceId g : mesh.vertexFaces(v)) {
        const Vec3 gn = mesh.faceNormal(g);
        // >= keeps neighbours within the angle; the owning face passes trivially.
        if (dot(faceN, gn) >= cosThreshold) {
            sum += gn * cornerAngle(mesh, g, v);
        }
    }
    const Vec3 n = normalized(sum);
    return lengthSquared(n) > 0.0f ? n : faceN;
}

// Barycentric weights of `p` relative to triangle (a, b, c), clamped to the
// triangle so a numerically-outside closest point never extrapolates the
// patch.
std::array<float, 3> barycentric(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 v0 = b - a;
    const Vec3 v1 = c - a;
    const Vec3 v2 = p - a;
    const float d00 = dot(v0, v0);
    const float d01 = dot(v0, v1);
    const float d11 = dot(v1, v1);
    const float d20 = dot(v2, v0);
    const float d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-20f) {
        return {1.0f, 0.0f, 0.0f};  // degenerate triangle: fall back to corner a
    }
    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0f - v - w;
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    w = std::clamp(w, 0.0f, 1.0f);
    const float total = u + v + w;
    return {u / total, v / total, w / total};
}

// Curved PN-triangle surface point for barycentric (u, v, w) over corners
// (P0, P1, P2) with corner normals (N0, N1, N2) — the cubic Bézier of
// Vlachos et al. (2001). Reduces exactly to the flat point when the corner
// normals equal the face normal (all edge tangents lie in the plane, so every
// wij = 0 and b111 = centroid).
Vec3 evaluatePn(const std::array<Vec3, 3>& p, const std::array<Vec3, 3>& n, float u, float v,
                float w) {
    auto edgePoint = [&](int i, int j) {
        const float wij = dot(p[static_cast<std::size_t>(j)] - p[static_cast<std::size_t>(i)],
                              n[static_cast<std::size_t>(i)]);
        return (p[static_cast<std::size_t>(i)] * 2.0f + p[static_cast<std::size_t>(j)] -
                n[static_cast<std::size_t>(i)] * wij) /
               3.0f;
    };
    const Vec3 b210 = edgePoint(0, 1);
    const Vec3 b120 = edgePoint(1, 0);
    const Vec3 b021 = edgePoint(1, 2);
    const Vec3 b012 = edgePoint(2, 1);
    const Vec3 b102 = edgePoint(2, 0);
    const Vec3 b201 = edgePoint(0, 2);
    const Vec3 e = (b210 + b120 + b021 + b012 + b102 + b201) / 6.0f;
    const Vec3 centroid = (p[0] + p[1] + p[2]) / 3.0f;
    const Vec3 b111 = e + (e - centroid) * 0.5f;

    const float u2 = u * u;
    const float v2 = v * v;
    const float w2 = w * w;
    return p[0] * (u2 * u) + p[1] * (v2 * v) + p[2] * (w2 * w) + b210 * (3.0f * u2 * v) +
           b120 * (3.0f * u * v2) + b021 * (3.0f * v2 * w) + b012 * (3.0f * v * w2) +
           b201 * (3.0f * u2 * w) + b102 * (3.0f * u * w2) + b111 * (6.0f * u * v * w);
}

}  // namespace

ReferenceSurface::ReferenceSurface(const Mesh& mesh, float smoothNormalDegrees)
    : m_bvh(mesh), m_smooth(smoothNormalDegrees > 0.0f) {
    if (!m_smooth) {
        return;
    }
    // acos is monotone decreasing: normals within `smoothNormalDegrees` have a
    // dot product >= cos(threshold). 180 deg -> cos = -1 -> every face joins.
    const float cosThreshold = std::cos(degreesToRadians(smoothNormalDegrees));
    m_patches.resize(mesh.faceCapacity());
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;  // only triangles carry a patch; others use the flat fallback
        }
        const std::vector<VertexId> verts = mesh.faceVertices(f);
        const Vec3 faceN = mesh.faceNormal(f);
        Patch& patch = m_patches[fi];
        patch.triangle = true;
        for (std::size_t c = 0; c < 3; ++c) {
            patch.corner[c] = mesh.position(verts[c]);
            patch.cornerNormal[c] = smoothedCornerNormal(mesh, verts[c], faceN, cosThreshold);
        }
    }
}

Vec3 ReferenceSurface::project(Vec3 query) const {
    const Bvh::ClosestHit hit = m_bvh.closestPoint(query);
    if (!m_smooth || hit.face.value >= m_patches.size()) {
        return hit.point;
    }
    const Patch& patch = m_patches[hit.face.value];
    if (!patch.triangle) {
        return hit.point;
    }
    const std::array<float, 3> bary =
        barycentric(hit.point, patch.corner[0], patch.corner[1], patch.corner[2]);
    return evaluatePn(patch.corner, patch.cornerNormal, bary[0], bary[1], bary[2]);
}

}  // namespace cyber::remesh
