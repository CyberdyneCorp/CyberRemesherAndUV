#include "cyber/render/overlays.hpp"

#include <algorithm>
#include <cmath>

namespace cyber::render {

namespace {

// An arbitrary unit vector orthogonal to `n` (assumed roughly unit length).
Vec3 anyPerpendicular(Vec3 n) {
    const Vec3 ref = (std::abs(n.x) < 0.9f) ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return normalized(cross(n, ref));
}

}  // namespace

std::array<Vec3, 3> triangleBarycentric() {
    return {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
}

std::vector<std::pair<Vec3, Vec3>> wireframeSegments(const Mesh& mesh) {
    std::vector<std::pair<Vec3, Vec3>> segments;
    segments.reserve(mesh.edgeCount());
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        segments.emplace_back(mesh.position(a), mesh.position(b));
    }
    return segments;
}

std::vector<Vec3> barycentricCorners(const Mesh& mesh) {
    const std::array<Vec3, 3> corner = triangleBarycentric();
    std::vector<Vec3> coords;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::size_t sides = mesh.faceSize(f);
        // Fan triangulation produces (sides - 2) triangles, 3 corners each.
        for (std::size_t t = 0; t + 2 < sides; ++t) {
            coords.push_back(corner[0]);
            coords.push_back(corner[1]);
            coords.push_back(corner[2]);
        }
    }
    return coords;
}

std::array<Vec3, 4> symmetryPlaneQuad(const SymmetryPlaneOverlay& plane) {
    const Vec3 n = normalized(plane.normal);
    if (length(n) <= 0.0f) {
        return {plane.point, plane.point, plane.point, plane.point};
    }
    const Vec3 u = anyPerpendicular(n) * plane.extent;
    const Vec3 v = normalized(cross(n, u)) * plane.extent;
    return {
        plane.point - u - v,
        plane.point + u - v,
        plane.point + u + v,
        plane.point - u + v,
    };
}

std::vector<Vec3> brushRing(const BrushRadiusOverlay& brush) {
    std::vector<Vec3> ring;
    const std::uint32_t segments = std::max<std::uint32_t>(brush.segments, 3);
    ring.reserve(segments);
    const Vec3 n = normalized(brush.normal);
    const Vec3 u = anyPerpendicular(n);
    const Vec3 v = normalized(cross(n, u));
    const float step = (2.0f * kPi) / static_cast<float>(segments);
    for (std::uint32_t i = 0; i < segments; ++i) {
        const float angle = step * static_cast<float>(i);
        const Vec3 offset = u * (std::cos(angle) * brush.radius) + v * (std::sin(angle) * brush.radius);
        ring.push_back(brush.center + offset);
    }
    return ring;
}

}  // namespace cyber::render
