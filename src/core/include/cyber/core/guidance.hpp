#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "cyber/core/mesh.hpp"

// User-drawn guidance for the remeshing pipeline (remeshing-pipeline spec,
// "Flow guides constrain the cross field" / "Painted density scales local
// sizing"). Two explicit, opt-in inputs — nothing here is inferred, which is
// what separates this from the descoped automatic curvature adaptivity.
//
//  - FlowGuide: an ordered polyline on or near the Target. Near it the cross
//    field is biased toward the guide's tangent, as a SOFT constraint: hard
//    pins (feature / boundary / crease alignment) still win, because turning
//    guides into hard constraints re-opens the singularity campaign.
//  - DensityField: a per-vertex or per-face scalar on the Target that
//    multiplies the local quads-per-unit-area. Since a quad covers
//    edge^2 of area, the documented sizing relation is
//
//        localEdgeLength = baseEdgeLength / sqrt(density)
//
//    so density composes with the area-derived targetQuadCount: painting 4.0
//    over a region asks for 4x the quads there, i.e. half the edge length.
//
// Guidance lives in core (not quadrangulate) so pipeline, C ABI, CLI and the
// network bridge can all name the same types.
namespace cyber::remesh {

class ReferenceSurface;

// Documented clamp ranges — the single source of truth, mirrored nowhere else
// (remeshing-parameters spec: "defaults and valid ranges live here").
inline constexpr float kGuideStrengthMin = 0.0f;
inline constexpr float kGuideStrengthMax = 1.0f;
inline constexpr float kDensityMin = 0.25f;
inline constexpr float kDensityMax = 4.0f;

// What a guide is asking for (remeshing-pipeline spec, "Flow guides constrain
// the cross field").
//
// The two are genuinely different requests, and conflating them is why "the
// loops didn't follow my stroke" is ambiguous today. An ORIENTATION guide says
// "line the field up this way around here" — a soft bias that competes with the
// smoothness term, so the loops nearby lean toward the stroke without any one
// of them being pinned to it. A TOPOLOGY guide says "put an actual edge loop
// HERE" — the stroke is meant to become a curve in the layout, not merely to
// influence the field around it.
//
// Orientation is the default, and is exactly today's behaviour: an existing
// guide, deserialized from an older schema or constructed without naming a
// mode, is an orientation guide and produces byte-identical output.
enum class GuideMode : std::uint8_t {
    Orientation,
    Topology,
};

struct FlowGuide {
    std::vector<Vec3> points;  // ordered polyline, at least 2 points
    float strength = 1.0f;     // [kGuideStrengthMin, kGuideStrengthMax]
    float radius = 0.0f;       // world-space influence radius, must be > 0
    GuideMode mode = GuideMode::Orientation;
    // The polyline closes back on its first point. Closed guides are what an
    // eye, mouth, shoulder, knee or wrist loop is; they ask for a continuous
    // ring rather than an open chain.
    bool closed = false;
};

// Painted sizing multiplier on the Target. Exactly one of the two arrays is
// populated; both empty means "no density".
struct DensityField {
    std::vector<float> vertexValues;  // indexed by the Target's live vertex order
    std::vector<float> faceValues;    // indexed by the Target's live face order

    [[nodiscard]] bool empty() const { return vertexValues.empty() && faceValues.empty(); }

    // True when values are supplied and every one of them is 1.0 (within float
    // noise): 1.0 is the identity of the sizing relation above, so such a paint
    // asks for nothing. Callers drop it rather than carry it, because merely
    // CARRYING guidance changes which backend runs (remeshing-pipeline spec:
    // "A density of 1.0 everywhere SHALL reproduce today's uniform sizing
    // byte-identically"). An empty field is not neutral — it is `empty()`.
    [[nodiscard]] bool isNeutral() const;
};

struct Guidance {
    std::vector<FlowGuide> guides;
    DensityField density;

    [[nodiscard]] bool empty() const { return guides.empty() && density.empty(); }
    // Guides asking for an actual edge loop, as opposed to a field bias.
    [[nodiscard]] std::size_t topologyGuideCount() const;
};

// A guide's contribution at a point: `weight` in [0,1] (0 = out of range) and
// the guide's local unit tangent.
struct GuideSample {
    float weight = 0.0f;
    Vec3 tangent{};
};

// Resolved, purely GEOMETRIC sampler. It snapshots guide segments (snapped
// onto the Target) and per-input-triangle density corners, then answers
// queries by closest-point + barycentric weights — exactly the pattern the
// adaptive ScaleField already uses. Being geometric, it survives
// triangulate / weld / orient / island split / isotropic remesh without any
// index remapping: every downstream stage just asks "what is the guidance at
// this world point".
class GuidanceField {
public:
    GuidanceField() = default;
    // `target` is the ORIGINAL input mesh the guidance was authored against.
    GuidanceField(const Mesh& target, const Guidance& guidance);
    ~GuidanceField();
    GuidanceField(GuidanceField&&) noexcept;
    GuidanceField& operator=(GuidanceField&&) noexcept;
    GuidanceField(const GuidanceField&) = delete;
    GuidanceField& operator=(const GuidanceField&) = delete;

    [[nodiscard]] bool hasGuides() const { return !m_guides.empty(); }
    [[nodiscard]] bool hasDensity() const { return !m_triangles.empty(); }
    [[nodiscard]] bool empty() const { return !hasGuides() && !hasDensity(); }
    [[nodiscard]] std::size_t guideCount() const { return m_guides.size(); }

    // The guides as authored, kept alongside the resolved sampler.
    //
    // The sampler is deliberately geometric — that is what lets it survive
    // triangulate / weld / island split / isotropic remesh without index
    // remapping. But a TOPOLOGY guide is not a point query: it has to be
    // projected onto the solve mesh as a whole polyline, and a backend has to
    // know which guides asked for that. So the authored list rides along,
    // read-only, for the backends that need the curve rather than the field.
    [[nodiscard]] const std::vector<FlowGuide>& sourceGuides() const { return m_sourceGuides; }
    [[nodiscard]] std::size_t topologyGuideCount() const;

    // Largest distance a supplied guide point had to move to reach the Target.
    // Reported rather than silently absorbed: a stroke drawn far off-surface
    // is a user error worth naming.
    [[nodiscard]] float maxSnapDistance() const { return m_maxSnapDistance; }

    // Strongest guide influence at `p`. `normal` (may be zero) gates the
    // sample on normal consistency so a guide drawn on one side of a thin
    // limb does not bleed to the other. Influence is EUCLIDEAN, not geodesic
    // — a documented limitation (docs/flow-guides.md).
    [[nodiscard]] GuideSample guideAt(Vec3 p, Vec3 normal = Vec3{}) const;

    // How many guides' influence volumes overlap the axis-aligned box
    // [lo, hi]. Used for the per-island guidance audit: it answers "did this
    // island even see a guide" without a per-face sweep.
    [[nodiscard]] std::size_t guidesReaching(Vec3 lo, Vec3 hi) const;

    // Painted density at `p`, clamped to [kDensityMin, kDensityMax]. 1.0 when
    // no density was supplied, so callers can multiply unconditionally.
    [[nodiscard]] float densityAt(Vec3 p) const;

private:
    std::vector<FlowGuide> m_sourceGuides;
    struct Segment {
        Vec3 a{}, b{};
        Vec3 normal{};  // Target normal at the segment midpoint (zero if unknown)
    };
    struct Guide {
        std::vector<Segment> segments;
        Vec3 boundsLo{}, boundsHi{};
        float strength = 1.0f;
        float radius = 0.0f;
    };
    struct DensityTriangle {
        std::array<Vec3, 3> position{};
        std::array<float, 3> value{1.0f, 1.0f, 1.0f};
    };

    std::unique_ptr<ReferenceSurface> m_surface;
    std::vector<Guide> m_guides;
    std::vector<DensityTriangle> m_triangles;  // indexed by the Target's FaceId; empty = no density
    float m_maxSnapDistance = 0.0f;
};

}  // namespace cyber::remesh
