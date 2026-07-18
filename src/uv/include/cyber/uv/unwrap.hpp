#pragma once

#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Conformal (LSCM) unwrap of a single island (uv-editing spec, "Gesture
// unwrap"): Least-Squares Conformal Maps (Lévy et al. 2002). Two corners are
// pinned automatically and the sparse least-squares conformal energy is
// solved with an in-house conjugate-gradient (CGLS) — no external solver.
namespace cyber::uv {

struct UnwrapOptions {
    // Upper bound on CG iterations; 0 selects an automatic bound from the
    // number of free variables.
    int maxIterations = 0;
    // Relative residual at which CG stops early.
    float tolerance = 1e-7f;
};

struct UnwrapResult {
    bool ok = false;
    // Island-local vertex order; uv[i] is the unwrapped position of
    // vertices[i].
    std::vector<VertexId> vertices;
    std::vector<Vec2> uv;
    int iterations = 0;
};

// Unwraps `island` (a set of faces) into UV space. Faces of any arity are
// fan-triangulated to build the conformal energy. Returns ok=false for a
// degenerate island (fewer than one face or three distinct vertices).
[[nodiscard]] UnwrapResult lscmUnwrap(const Mesh& mesh, std::span<const FaceId> island,
                                      const UnwrapOptions& options = {});

// Writes an unwrap result back into the per-loop "uv" attribute for the
// island's corners (creating the attribute if needed).
void writeIslandUv(Mesh& mesh, std::span<const FaceId> island, const UnwrapResult& result);

// Convenience: unwrap the island and store the result in the "uv" attribute.
// Returns false on a degenerate island.
bool unwrapIslandToUv(Mesh& mesh, std::span<const FaceId> island,
                      const UnwrapOptions& options = {});

}  // namespace cyber::uv
