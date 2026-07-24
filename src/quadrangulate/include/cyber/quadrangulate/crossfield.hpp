#pragma once

#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/mesh.hpp"

// Per-face cross (4-symmetry / N-RoSy, N=4) field over a triangle mesh — the
// direction guidance a field-aligned quadrangulator follows (remeshing-pipeline
// spec, "frame field"). The field is smoothed by a parallel-transported
// neighbour-averaging operator applied as a sparse matrix-vector product
// dispatched through the compute-acceleration layer, so on a GPU backend the
// field smoothing runs on the GPU (task 5.8 — "GPU dispatch of hot spots:
// field smoothing, spmv").
namespace cyber::remesh {

class CrossField {
public:
    // A cross at face f is the four directions {theta, theta+90, +180, +270}
    // in the face's tangent frame; stored as the unit complex u = e^{i*4*theta}
    // = (real, imag) so averaging is linear.
    std::vector<Vec3> tangent;    // per-face unit reference tangent (in face plane)
    std::vector<Vec3> bitangent;  // per-face normal x tangent
    std::vector<float> real;      // per-face cos(4*theta)
    std::vector<float> imag;      // per-face sin(4*theta)

    [[nodiscard]] std::size_t size() const { return real.size(); }
    // Representative cross direction (theta) of face f in world space.
    [[nodiscard]] Vec3 direction(FaceId f) const;
    // theta in [0, pi/2) for face f.
    [[nodiscard]] float angle(FaceId f) const;
};

// Computes a feature-aligned smoothed cross field. Faces incident to a feature
// or boundary edge are constrained to align with it; the interior relaxes
// toward those constraints over `iterations` smoothing steps. `backend`
// carries every spmv (default backend = CPU reference; a GPU backend
// accelerates it transparently).
//
// `creaseAlignDegrees` widens the set of edges the field aligns to WITHOUT widening the set that
// becomes a hard seam: any interior edge whose face-normal angle exceeds it pins its faces to the
// crease direction, while `Mesh::isFeatureEdge` (and so the seam set, period jumps and cut graph)
// is untouched. **Defaults to 0 (disabled).** It is a measured win on curved CAD (fandisk: feature
// -5.8%, edge CV -9.1%, irregular -8.8% over 7 densities) but a measured REGRESSION on flat CAD
// bounded by creases (a subdivided cube: edge CV 0.201 -> 0.397, slivers 0.1% -> 1.2%), where
// pinning every face of a flat panel to its four differently-oriented boundary creases
// over-constrains a field that should stay smooth. It needs a flatness gate before it can default
// on; until then callers opt in explicitly. See docs/ROADMAP.md Phase 3 lever c2.
[[nodiscard]] CrossField computeCrossField(const Mesh& mesh, int iterations,
                                           accel::IBackend& backend,
                                           float creaseAlignDegrees = 0.0f);

// Alternative cross field derived from the multiresolution per-vertex 4-RoSy
// orientation field (computePositionField): the coarse-to-fine hierarchy places
// singularities globally, which a single-level face smoothing gets stuck on —
// fewer, better-placed cones on thin high-curvature features (e.g. the bunny
// ears). The smoothed vertex orientation is projected onto each face and encoded
// as the per-face cross; faces touching a feature/boundary edge are re-pinned to
// it exactly, matching computeCrossField. Gated behind CYBER_QC_CROSSFIELD_MULTIRES
// so the stock seamless path (and the field-aligned engine) are unchanged.
[[nodiscard]] CrossField computeCrossFieldFromOrientation(const Mesh& mesh, int iterations);

}  // namespace cyber::remesh
