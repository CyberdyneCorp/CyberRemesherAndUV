#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>

#include "cyber/core/quadrangulate.hpp"
#include "cyber/core/region_solve.hpp"
#include "cyber/quadrangulate/crossfield.hpp"

namespace cyber::remesh {

// Cross-field-guided quadrangulator (QuadCover-lite). It computes a smoothed
// 4-symmetry field (see crossfield.hpp), then pairs triangles into quads
// preferring merges whose removed edge is diagonal to the field — so the
// surviving quad edges follow the field's flow — and finally simplifies the
// quad graph by dissolving valence-2 doublets (task 5.5). This is a genuine
// field-aligned quadrangulation without the full seamless MIQ parameterization
// / integer-isoline extraction of exploragram's QuadCover.
//
// Implements the core IQuadrangulator seam; the pipeline uses it when injected
// (the CLI does), while the default remains greedy pairing so recorded golden
// baselines stay stable.
std::unique_ptr<IQuadrangulator> makeFieldAlignedQuadrangulator(int fieldIterations = 30);

// Guided variant (add-weave-guide-field-steering): the quadrangulator steers
// its cross field toward `guides` (world-space direction samples), so quad flow
// follows user guide strokes / tagged loops. Empty guides == the unguided call.
std::unique_ptr<IQuadrangulator> makeFieldAlignedQuadrangulator(int fieldIterations,
                                                                OrientationGuides guides);

// Region-aware variant (add-weave-regional-solve). `region` must outlive the
// returned quadrangulator; null is exactly the call above.
//
// What it changes: PRECEDENCE. crossfield.cpp already pins a feature/boundary
// face's field as an exact Dirichlet condition (:83 seeds it, :186-189 never
// writes a constrained face back), and a region solve feature-tags its whole
// interface — so the interface field is already hard-pinned. But the external
// guide loop (:102-119) runs AFTER the feature loop and does not check
// `constrained`, deliberately, so "the user's intent wins" on free surface.
// On an interface face that is wrong: it erases the prescription the region
// solve depends on. The fix is to filter the guides, not to add a constraint
// tier to crossfield.hpp — which patch 0003 owns, and enlarging its conflict
// surface buys no behavioural difference.
//
// Deliberately NOT included: an interface-first triangle-pairing pass. It was
// built and measured during the task-0 spike and changed interface conformance
// by exactly zero, because pairing decides WHICH triangles merge, not how many
// quads meet a given vertex. See the change's design.md ADDENDUM 2.
std::unique_ptr<IQuadrangulator> makeFieldAlignedQuadrangulator(int fieldIterations,
                                                                OrientationGuides guides,
                                                                const RegionSolve* region);

// Post-quadrangulation valence cleanup (roadmap 5.4/5.5 depth — NOT a seamless
// MIQ port). After pairing and doublet removal, rotates the diagonal shared by
// two quads whenever the rotation strictly lowers the total |valence - 4| over
// the four affected vertices, keeping every incident face a quad and the mesh
// manifold. Each candidate is fully guarded — the two shared-edge endpoints and
// the two receiving corners must be interior (only manifold, non-feature edges)
// and quad-only, the new diagonal must not already exist, and the two faces must
// stay quads — so a rejected move is a strict no-op and the mesh always stays
// valid. An optional cross field breaks ties toward field-aligned edges. Runs at
// most `maxPasses` deterministic sweeps and returns the number of rotations
// applied. A conservative no-op is preferred over any op that could corrupt the
// mesh.
std::size_t quadValenceCleanup(Mesh& mesh, const CrossField* field = nullptr, int maxPasses = 4);

}  // namespace cyber::remesh
