#pragma once
#include <unordered_map>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/core/region_solve.hpp"

// Post-solve verification for a region solve (add-weave-regional-solve, task 7).
//
// TWO TIERS, and the split is the whole point.
//
//   REFUSE — everything that would break EXACT LANDING: a prescribed vertex
//   dead or moved, a lost interface edge, a corrupted frozen face ring. The
//   task-0 spike measured all three as always-holding across flat, L-shaped and
//   domed fixtures at 1x and 4x density, so a failure here is a REGRESSION, not
//   a hard problem. Refusing costs nothing and catches real breakage — removing
//   the SplitPass guard, for instance, corrupts 20 frozen rings and loses 16
//   interface edges on the 6x6 fixture while leaving every POSITION correct.
//
//   REPORT — interface irregularity, the index-identity residual, and triangles
//   touching the interface. Never blocks publication. Forcing irregularity to
//   zero is a coupled degree-constrained matching over the interface ring: a
//   merge across (b,x) decrements the face count at BOTH endpoints, so the ring
//   is coupled and every local pass deadlocks. Three were built and measured
//   during the spike; the best left 3 of 16 vertices unresolved on the simplest
//   fixture. Gating on it rejected EVERY fixture, which would have shipped a
//   solver that never solves. See the change's design.md ADDENDUM 2, and task
//   5.3a for the guarantee.
namespace cyber::remesh {

// The prescription, captured BEFORE the solve mutates anything.
struct InterfaceSnapshot {
    std::unordered_map<Index, Vec3> prescribedPositions;        // pinned vertex -> position
    std::unordered_map<Index, std::vector<Index>> frozenRings;  // frozen face -> vertex ring
    std::vector<std::pair<Index, Index>> interfaceEdges;        // sorted, endpoints ordered
};

[[nodiscard]] InterfaceSnapshot captureInterface(const Mesh& mesh, const RegionSolve& region);

struct ConformanceResult {
    // REFUSE tier — non-empty/non-zero means the solve must not be published.
    std::vector<VertexId> movedOrDeadPrescribed;
    std::vector<FaceId> corruptedFrozenFaces;
    std::vector<std::pair<Index, Index>> lostInterfaceEdges;

    // REPORT tier — information for the caller.
    std::vector<VertexId> irregularInterface;
    int indexResidual = 0;  // 0 means the discrete index identity balances
    std::size_t interfaceTriangles = 0;

    [[nodiscard]] bool exact() const {
        return movedOrDeadPrescribed.empty() && corruptedFrozenFaces.empty() &&
               lostInterfaceEdges.empty();
    }
    // A human-readable summary of the REFUSE tier only.
    [[nodiscard]] std::string describeFailure() const;
};

[[nodiscard]] ConformanceResult verifyInterfaceConformance(const Mesh& mesh,
                                                           const RegionSolve& region,
                                                           const InterfaceSnapshot& before);

}  // namespace cyber::remesh
