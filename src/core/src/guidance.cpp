#include "cyber/core/guidance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cyber/core/reference_surface.hpp"

namespace cyber::remesh {

namespace {

// Barycentric weights of p projected into the triangle, clamped inside it
// (same construction the adaptive ScaleField uses).
std::array<float, 3> guideBarycentric(Vec3 p, const std::array<Vec3, 3>& tri) {
    const Vec3 v0 = tri[1] - tri[0];
    const Vec3 v1 = tri[2] - tri[0];
    const Vec3 v2 = p - tri[0];
    const float d00 = dot(v0, v0);
    const float d01 = dot(v0, v1);
    const float d11 = dot(v1, v1);
    const float d20 = dot(v2, v0);
    const float d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-20f) {
        return {1.0f, 0.0f, 0.0f};
    }
    const float v = std::clamp((d11 * d20 - d01 * d21) / denom, 0.0f, 1.0f);
    const float w = std::clamp((d00 * d21 - d01 * d20) / denom, 0.0f, 1.0f - v);
    return {1.0f - v - w, v, w};
}

// Squared distance from p to segment [a,b], and the closest point.
float segmentDistanceSq(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const float len2 = dot(ab, ab);
    const float t = len2 > 1e-20f ? std::clamp(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    const Vec3 q = a + ab * t;
    return dot(p - q, p - q);
}

}  // namespace

std::size_t Guidance::topologyGuideCount() const {
    return static_cast<std::size_t>(
        std::count_if(guides.begin(), guides.end(),
                      [](const FlowGuide& g) { return g.mode == GuideMode::Topology; }));
}

bool DensityField::isNeutral() const {
    if (empty()) {
        return false;
    }
    constexpr float kNeutralEpsilon = 1e-6f;
    const auto neutral = [](float v) { return std::fabs(v - 1.0f) <= kNeutralEpsilon; };
    return std::all_of(vertexValues.begin(), vertexValues.end(), neutral) &&
           std::all_of(faceValues.begin(), faceValues.end(), neutral);
}

GuidanceField::~GuidanceField() = default;
GuidanceField::GuidanceField(GuidanceField&&) noexcept = default;
GuidanceField& GuidanceField::operator=(GuidanceField&&) noexcept = default;

std::size_t GuidanceField::topologyGuideCount() const {
    return static_cast<std::size_t>(
        std::count_if(m_sourceGuides.begin(), m_sourceGuides.end(),
                      [](const FlowGuide& g) { return g.mode == GuideMode::Topology; }));
}

GuidanceField::GuidanceField(const Mesh& target, const Guidance& guidance) {
    if (guidance.empty()) {
        return;
    }
    m_sourceGuides = guidance.guides;
    m_surface = std::make_unique<ReferenceSurface>(target, 0.0f);

    // Guides: snap every point onto the Target so a stroke drawn slightly off
    // the surface still measures its influence radius against real geometry.
    // The snap distance is recorded so the caller can report a stroke that was
    // authored far away instead of silently mis-scaling it.
    for (const FlowGuide& raw : guidance.guides) {
        if (raw.points.size() < 2 || !(raw.radius > 0.0f)) {
            continue;  // validateGuidance rejects these before we get here
        }
        Guide guide;
        guide.strength = raw.strength;
        guide.radius = raw.radius;
        std::vector<Vec3> snapped;
        snapped.reserve(raw.points.size());
        for (const Vec3 p : raw.points) {
            const Vec3 q = m_surface->empty() ? p : m_surface->project(p);
            m_maxSnapDistance = std::max(m_maxSnapDistance, length(q - p));
            snapped.push_back(q);
        }
        guide.boundsLo = snapped[0];
        guide.boundsHi = snapped[0];
        for (std::size_t i = 1; i < snapped.size(); ++i) {
            if (lengthSquared(snapped[i] - snapped[i - 1]) <= 0.0f) {
                continue;  // duplicate point: no direction to contribute
            }
            Segment seg;
            seg.a = snapped[i - 1];
            seg.b = snapped[i];
            const Bvh::ClosestHit hit = m_surface->empty()
                                            ? Bvh::ClosestHit{}
                                            : m_surface->closest(lerp(seg.a, seg.b, 0.5f));
            if (hit.face.valid() && target.isAlive(hit.face)) {
                seg.normal = normalized(target.faceNormal(hit.face));
            }
            guide.segments.push_back(seg);
            guide.boundsLo = min(guide.boundsLo, snapped[i]);
            guide.boundsHi = max(guide.boundsHi, snapped[i]);
        }
        if (guide.segments.empty()) {
            continue;
        }
        const Vec3 pad{guide.radius, guide.radius, guide.radius};
        guide.boundsLo = guide.boundsLo - pad;
        guide.boundsHi = guide.boundsHi + pad;
        m_guides.push_back(std::move(guide));
    }

    // Density: snapshot per-input-triangle corner values, so a query is one
    // BVH closest-point plus a barycentric blend.
    if (!guidance.density.empty()) {
        const bool perVertex = !guidance.density.vertexValues.empty();
        m_triangles.resize(target.faceCapacity());
        std::size_t faceOrdinal = 0;
        std::size_t vertexOrdinal = 0;
        std::vector<float> perVertexValue;
        if (perVertex) {
            perVertexValue.assign(target.vertexCapacity(), 1.0f);
            for (Index vi = 0; vi < target.vertexCapacity(); ++vi) {
                if (!target.isAlive(VertexId{vi})) {
                    continue;
                }
                if (vertexOrdinal < guidance.density.vertexValues.size()) {
                    perVertexValue[vi] = guidance.density.vertexValues[vertexOrdinal];
                }
                ++vertexOrdinal;
            }
        }
        for (Index fi = 0; fi < target.faceCapacity(); ++fi) {
            const FaceId f{fi};
            if (!target.isAlive(f)) {
                continue;
            }
            const std::vector<VertexId> verts = target.faceVertices(f);
            DensityTriangle& tri = m_triangles[fi];
            float faceValue = 1.0f;
            if (!perVertex && faceOrdinal < guidance.density.faceValues.size()) {
                faceValue = guidance.density.faceValues[faceOrdinal];
            } else if (perVertex && verts.size() != 3) {
                // An n-gon is fan-triangulated inside the BVH, so a single
                // corner triple cannot span it: carry the face average
                // (constant over the polygon) rather than extrapolating.
                float sum = 0.0f;
                for (const VertexId v : verts) {
                    sum += perVertexValue[v.value];
                }
                faceValue = verts.empty() ? 1.0f : sum / static_cast<float>(verts.size());
            }
            const bool interpolate = perVertex && verts.size() == 3;
            for (std::size_t k = 0; k < 3; ++k) {
                const VertexId v = verts[std::min(k, verts.size() - 1)];
                tri.position[k] = target.position(v);
                tri.value[k] = interpolate ? perVertexValue[v.value] : faceValue;
            }
            ++faceOrdinal;
        }
    }
}

GuideSample GuidanceField::guideAt(Vec3 p, Vec3 normal) const {
    GuideSample best;
    const bool haveNormal = lengthSquared(normal) > 0.0f;
    for (const Guide& guide : m_guides) {
        if (p.x < guide.boundsLo.x || p.x > guide.boundsHi.x || p.y < guide.boundsLo.y ||
            p.y > guide.boundsHi.y || p.z < guide.boundsLo.z || p.z > guide.boundsHi.z) {
            continue;  // AABB early reject
        }
        const float r2 = guide.radius * guide.radius;
        for (const Segment& seg : guide.segments) {
            const float d2 = segmentDistanceSq(p, seg.a, seg.b);
            if (d2 >= r2) {
                continue;
            }
            // Normal-consistency gate: a guide lying on the far side of a thin
            // limb faces the other way, so it must not bleed through. Only
            // rejects OPPOSING normals; a tight fold whose normals agree is
            // the documented Euclidean-influence limitation.
            if (haveNormal && lengthSquared(seg.normal) > 0.0f &&
                dot(normalized(normal), seg.normal) <= 0.0f) {
                continue;
            }
            // Smooth falloff (1 - (d/r)^2)^2: 1 on the stroke, 0 with zero
            // slope at the radius, so the bias never steps.
            const float t = 1.0f - d2 / r2;
            const float w = guide.strength * t * t;
            if (w > best.weight) {
                best.weight = w;
                best.tangent = normalized(seg.b - seg.a);
            }
        }
    }
    return best;
}

std::size_t GuidanceField::guidesReaching(Vec3 lo, Vec3 hi) const {
    std::size_t count = 0;
    for (const Guide& guide : m_guides) {
        const bool disjoint = hi.x < guide.boundsLo.x || lo.x > guide.boundsHi.x ||
                              hi.y < guide.boundsLo.y || lo.y > guide.boundsHi.y ||
                              hi.z < guide.boundsLo.z || lo.z > guide.boundsHi.z;
        if (!disjoint) {
            ++count;
        }
    }
    return count;
}

float GuidanceField::densityAt(Vec3 p) const {
    if (m_triangles.empty() || m_surface == nullptr) {
        return 1.0f;
    }
    const Bvh::ClosestHit hit = m_surface->closest(p);
    if (!hit.face.valid() || hit.face.value >= m_triangles.size()) {
        return 1.0f;
    }
    const DensityTriangle& tri = m_triangles[hit.face.value];
    const std::array<float, 3> w = guideBarycentric(hit.point, tri.position);
    const float value = w[0] * tri.value[0] + w[1] * tri.value[1] + w[2] * tri.value[2];
    return std::clamp(value, kDensityMin, kDensityMax);
}

}  // namespace cyber::remesh
