#pragma once
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "cyber/core/mesh.hpp"

// Regional prescribed-boundary solve (openspec add-weave-regional-solve, tasks
// 1-3). A region solve rewrites a SUBSET of a mesh's faces in place while the
// complement — the "frozen" faces — keeps its exact geometry, topology and
// element ids, so the solved patch meets the existing cage on a prescribed
// boundary rather than replacing the whole mesh.
//
// This header carries the mask pair the solve is scoped by, plus the interface
// description the caller needs to judge the result.
//
// Two invariants make the masks safe to hold across a whole solve:
//
//   Invariant F — a frozen face is never removed by any pass, so its id never
//   enters m_freeFaces (mesh.cpp:86-90) and can never be recycled
//   (mesh.cpp:44-46). `active(f)` therefore stays correct for the entire run
//   even while region face ids churn freely.
//
//   Invariant P — a pinned vertex has at least one incident feature edge,
//   hence isFeatureVertex is true for it (isotropic.cpp:31-38), hence it is
//   never collapsed, dissolved, rotation-eligible or smoothed. It is never
//   freed, so its id is never recycled and the mask stays valid.
//
// MEASURED, not assumed: with these masks and the SplitPass guard in place, a
// full region solve leaves every pinned position bitwise identical and every
// frozen face ring identical, at 1x and 4x target density, on flat, L-shaped
// and domed fixtures. Removing the SplitPass guard corrupts 20 frozen rings and
// loses 16 interface edges on the 6x6 fixture at 4x. See the change's spike/.
//
// What this does NOT give you: interface REGULARITY. `requiredInRegion` is a
// prescription the solve is measured against, not one it is guaranteed to meet
// — forcing it is a coupled degree-constrained matching over the interface ring
// (three local passes were built and all deadlock; see the change's design.md
// ADDENDUM 2). Report it; do not refuse on it.
namespace cyber::remesh {

enum class RegionSolveStatus {
    Ok,
    EmptyRegion,          // no faces named
    InvalidFace,          // a named face is dead, out of range, or repeated
    WholeMesh,            // the region is every live face — use the whole-mesh path
    Disconnected,         // the named faces are not face-connected
    CoincidentVertices,   // weldCoincidentVertices cannot run in region mode
    InconsistentWinding,  // orientFacesConsistently cannot run in region mode
};

[[nodiscard]] std::string_view describe(RegionSolveStatus status);

struct RegionSolve {
    std::vector<char> frozenFace;    // indexed by FaceId::value
    std::vector<char> vertexPinned;  // indexed by VertexId::value

    // Ordered interface rings: vertices where the region meets frozen topology.
    // An interface edge has exactly one incident face in the region, so it is
    // NOT a boundary edge (both its faces are alive) — which is precisely why
    // retopo::boundaryChain cannot see it and this walk exists.
    std::vector<std::vector<VertexId>> interfaceLoops;

    // Per interface vertex: the total valence implied by the surrounding cage
    // (caller-overridable), and how many of those faces the solve must supply.
    std::unordered_map<Index, int> targetValence;
    std::unordered_map<Index, int> requiredInRegion;

    // 4*chi - sum(2 - q_in) over the interface. Reported, not enforced.
    int interiorIndexBudget = 0;

    [[nodiscard]] bool empty() const { return frozenFace.empty(); }

    // Faces and vertices created DURING the solve are past the masks' end and
    // are region elements by construction, so out-of-range is active/unpinned.
    [[nodiscard]] bool frozen(FaceId f) const {
        return f.value < frozenFace.size() && frozenFace[f.value] != 0;
    }
    [[nodiscard]] bool active(FaceId f) const { return !frozen(f); }
    [[nodiscard]] bool pinned(VertexId v) const {
        return v.value < vertexPinned.size() && vertexPinned[v.value] != 0;
    }

    // True for a live edge with exactly one incident face in the region. The
    // region-scoped analogue of retopo::isBoundaryEdge.
    [[nodiscard]] bool isInterfaceEdge(const Mesh& mesh, EdgeId e) const;
};

struct RegionSolveResult {
    RegionSolveStatus status = RegionSolveStatus::Ok;
    RegionSolve region;
    [[nodiscard]] bool ok() const { return status == RegionSolveStatus::Ok; }
};

// Builds the mask pair for `regionFaces` and prepares `mesh` for a region
// solve. On a non-Ok status the mesh is left UNTOUCHED and the region is empty.
//
// On success this MUTATES `mesh`: it triangulates the ACTIVE faces only (the
// frozen complement keeps its quads and n-gons) and rewrites feature flags.
//
// Step order is load-bearing: tagFeatureEdges rewrites `feature` on EVERY alive
// edge (mesh_diagnostics.cpp:182-203), so interface tagging must follow it.
//
// `valenceOverrides` maps a VertexId::value to a caller-prescribed TOTAL
// valence, so a deliberately authored pole on the interface is not reported as
// a defect. Null = derive every prescription from the cage.
RegionSolveResult buildRegionSolve(Mesh& mesh, std::span<const FaceId> regionFaces,
                                   float sharpEdgeDegrees,
                                   const std::unordered_map<Index, int>* valenceOverrides = nullptr);

}  // namespace cyber::remesh
