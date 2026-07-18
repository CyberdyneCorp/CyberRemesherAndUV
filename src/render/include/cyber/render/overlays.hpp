#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Viewport overlay descriptors (viewport-rendering spec, task 7.3). These are
// plain data: they describe *what* to draw on top of the shaded mesh. The
// geometry builders (wireframe segments, brush ring, symmetry quad) are real,
// portable C++ and unit-testable; a backend turns the descriptors into draws.
namespace cyber::render {

// Barycentric single-pass wireframe: each triangle corner carries a canonical
// barycentric coordinate and the fragment shader draws edges where any
// component approaches zero. Avoids a separate line pass and z-fighting.
struct WireframeOverlay {
    Vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
    float lineWidthPx = 1.0f;
    bool enabled = true;
    bool depthBias = true;  // pull toward the camera to beat z-fighting
};

// Canonical barycentric coordinates for the three corners of a triangle.
[[nodiscard]] std::array<Vec3, 3> triangleBarycentric();

// One world-space line segment (a, b) per mesh edge, for a classic wireframe
// overlay. Deterministic, ordered by edge id.
[[nodiscard]] std::vector<std::pair<Vec3, Vec3>> wireframeSegments(const Mesh& mesh);

// Per-corner barycentric attribute for a triangulated view of `mesh`, aligned
// with a fan triangulation of each face. Size is 3 * (triangle count).
[[nodiscard]] std::vector<Vec3> barycentricCorners(const Mesh& mesh);

// Pinned / constrained vertices (retopo guides, UV pins) drawn as billboards.
struct PinOverlay {
    std::vector<Vec3> positions;
    Vec4 color{1.0f, 0.85f, 0.1f, 1.0f};
    float radiusPx = 6.0f;
    bool enabled = true;
};

// Highlighted edge loops / rings drawn as thick polylines.
struct LoopOverlay {
    std::vector<std::vector<Vec3>> polylines;
    Vec4 color{0.15f, 0.6f, 1.0f, 1.0f};
    float lineWidthPx = 2.0f;
    bool closed = false;
    bool enabled = true;
};

// The mirror plane used by symmetric editing, drawn as a translucent quad plus
// its normal.
struct SymmetryPlaneOverlay {
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{1.0f, 0.0f, 0.0f};
    float extent = 1.0f;  // half-size of the quad
    Vec4 color{0.4f, 0.9f, 0.6f, 0.25f};
    bool enabled = false;
};

// Four corner positions of the symmetry plane quad, ordered CCW around the
// normal. Returns a degenerate (all-equal) quad for a zero normal.
[[nodiscard]] std::array<Vec3, 4> symmetryPlaneQuad(const SymmetryPlaneOverlay& plane);

// The sculpt/retopo brush footprint, drawn as a ring lying on the surface at
// the cursor hit (center + surface normal).
struct BrushRadiusOverlay {
    Vec3 center{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float radius = 1.0f;
    Vec4 color{1.0f, 1.0f, 1.0f, 0.9f};
    std::uint32_t segments = 48;
    bool enabled = true;
};

// Ring vertices for a brush overlay, laid in the plane defined by its normal.
// Returns `segments` points (caller closes the loop).
[[nodiscard]] std::vector<Vec3> brushRing(const BrushRadiusOverlay& brush);

// The full set of overlays composited for one frame.
struct OverlaySet {
    WireframeOverlay wireframe{};
    PinOverlay pins{};
    LoopOverlay loops{};
    SymmetryPlaneOverlay symmetry{};
    BrushRadiusOverlay brush{};
};

}  // namespace cyber::render
