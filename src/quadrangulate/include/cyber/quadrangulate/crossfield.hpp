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
// is untouched. 0 disables it. Pins are applied only where the face's own-side neighbourhood is
// genuinely curved: a planar panel's cross field is degenerate, so pinning its border imposes
// arbitrary structure (ungated, that took a subdivided cube's edge CV 0.201 -> 0.397). With the
// planarity gate, fandisk improves (feature -2.9%, median +0.25, edge CV -0.008 over 7 densities)
// and flat CAD is left bit-identical. See docs/ROADMAP.md Phase 3 lever c2.
//
// `creaseAlignSupport` (optional, indexed by EdgeId) restricts crease-ALIGNMENT pins to edges
// flagged 1: the resolution-aware feature demotion (quadcover_extractor.cpp) flags only crease
// edges tracing the ORIGINAL mesh's resolvable sharp network, so sub-resolution wrinkle
// dihedrals on a coarse organic remesh stop freezing the field (nefertiti@4000: 43% of faces
// pinned by wrinkle alignment, 1297 cones). Null (the default) keeps every crease pin —
// historical behavior. Feature/boundary pins are never gated by it.
[[nodiscard]] CrossField computeCrossField(const Mesh& mesh, int iterations,
                                           accel::IBackend& backend,
                                           float creaseAlignDegrees = 45.0f,
                                           const std::vector<char>* creaseAlignSupport = nullptr);

// Alternative cross field derived from the multiresolution per-vertex 4-RoSy
// orientation field (computePositionField): the coarse-to-fine hierarchy places
// singularities globally, which a single-level face smoothing gets stuck on —
// fewer, better-placed cones on thin high-curvature features (e.g. the bunny
// ears). The smoothed vertex orientation is projected onto each face, pinned
// exactly as computeCrossField pins (feature/boundary alignment, crease pins,
// planar flood fill), and then RELAXED with the same converged transport
// smoothing the stock path runs — seeded from the hierarchy instead of theta=0,
// so the coarse cone placement is kept while the fine scale is as converged as
// stock (the raw vertex-to-face hand-off measurably regressed edge CV / normal
// error). Gated behind CYBER_QC_CROSSFIELD_MULTIRES so the stock seamless path
// (and the field-aligned engine) are unchanged.
[[nodiscard]] CrossField computeCrossFieldFromOrientation(
    const Mesh& mesh, int iterations, accel::IBackend& backend, float creaseAlignDegrees = 45.0f,
    const std::vector<char>* creaseAlignSupport = nullptr);

}  // namespace cyber::remesh
