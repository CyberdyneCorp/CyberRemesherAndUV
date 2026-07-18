#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"

// Pipeline-stage preview descriptors (viewport-rendering spec, task 7.4). The
// viewport can show any stage of the remesh: the Source cage, the Isotropic
// intermediate, the Param (UV cross-field) stage, and the final Result quad
// mesh, each with stage-specific decorations (singularities, cross pattern).
// Descriptors and their small geometry builders are real, portable C++.
namespace cyber::render {

enum class PreviewStage : std::uint8_t {
    Source,      // input triangle soup / cage
    Isotropic,   // isotropic triangle remesh
    Param,       // parametrization / cross field
    Result,      // final quad-dominant mesh
};

[[nodiscard]] const char* toString(PreviewStage stage);

// A quad-mesh singularity (irregular vertex). Valence 4 is regular; 3 and 5 are
// the common cone points a quad remesher produces. Colored by valence so the
// viewport reads the field at a glance.
struct SingularityMarker {
    Vec3 position{0.0f, 0.0f, 0.0f};
    std::int32_t valence = 4;
};

// Distinct, stable color for a singularity valence: blue for low (< 4), regular
// grey for 4, red for high (> 4), scaling with how far from regular it is.
[[nodiscard]] Vec4 singularityColor(std::int32_t valence);
[[nodiscard]] inline bool isSingular(std::int32_t valence) { return valence != 4; }

// One sample of the 4-way rotational-symmetry cross field used during
// parametrization: two orthogonal in-plane directions at a surface point.
struct CrossFieldSample {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 dir0{1.0f, 0.0f, 0.0f};  // primary flow direction (unit)
    Vec3 dir1{0.0f, 0.0f, 1.0f};  // orthogonal companion (unit)
};

// Line segments (a, b) drawing the 4-way cross for each sample, scaled by
// `length`. Four arms per sample (+/- dir0, +/- dir1).
[[nodiscard]] std::vector<std::pair<Vec3, Vec3>> crossFieldSegments(
    const std::vector<CrossFieldSample>& samples, float length);

// UV-space preview of the integer-grid / cross pattern the parametrization maps
// onto the surface: a lattice of iso-lines in [uMin,uMax] x [vMin,vMax].
struct CrossPatternPreview {
    Vec2 uvMin{0.0f, 0.0f};
    Vec2 uvMax{1.0f, 1.0f};
    std::uint32_t linesU = 16;  // iso-lines of constant u
    std::uint32_t linesV = 16;  // iso-lines of constant v
    Vec4 color{0.2f, 0.8f, 0.9f, 1.0f};
};

// UV-space line segments for the cross/grid pattern (2D). Each entry is a
// segment endpoint pair in UV space.
[[nodiscard]] std::vector<std::pair<Vec2, Vec2>> crossPatternSegments(
    const CrossPatternPreview& pattern);

struct StagePreviewDescriptor {
    PreviewStage stage = PreviewStage::Source;

    // Shading toggles resolved per stage by the viewport.
    bool showWireframe = true;
    bool showSingularities = false;  // meaningful for Param/Result
    bool showCrossField = false;     // meaningful for Param
    bool showUvGrid = false;         // meaningful for Param/Result

    std::vector<SingularityMarker> singularities;
    std::vector<CrossFieldSample> crossField;
    CrossPatternPreview crossPattern{};

    // Base tint applied to the shaded mesh for this stage (helps distinguish
    // stages in a split view).
    Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
};

// A sensible default descriptor for each stage (toggles + tint preset).
[[nodiscard]] StagePreviewDescriptor defaultDescriptor(PreviewStage stage);

}  // namespace cyber::render
