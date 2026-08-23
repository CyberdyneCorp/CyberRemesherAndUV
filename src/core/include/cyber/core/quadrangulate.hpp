#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "cyber/core/guidance.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/progress.hpp"

namespace cyber::remesh {

// Stage seam for turning an isotropically remeshed triangle island into a
// quad-dominant mesh (design D2: the parameterization solver is swappable).
//
// Implementations planned:
//  - GreedyPairing (below): local triangle-pairing baseline. Ships first so
//    the pipeline, CLI and tests run end-to-end.
//  - QuadCover (task 5.4): frame-field + mixed-integer global
//    parameterization with integer-isoline extraction (exploragram port) —
//    the production-quality path required by the remeshing-pipeline spec.
class IQuadrangulator {
public:
    virtual ~IQuadrangulator() = default;

    struct Outcome {
        bool success = false;
        bool cancelled = false;
        std::string failureReason;
    };

    // Rewrites `mesh` in place. Feature edges must be tagged beforehand;
    // implementations respect them as hard constraints.
    virtual Outcome quadrangulate(Mesh& mesh, float targetEdgeLength, ProgressSink* progress,
                                  const CancelToken* cancel) = 0;

    // Offer user-drawn guidance to this backend. The DEFAULT implementation
    // DECLINES and sets `reason`, so a backend with no guide support reports
    // loudly instead of silently ignoring the input (remeshing-pipeline spec,
    // "Guidance is honored loudly or rejected loudly"). `field` outlives the
    // quadrangulate() call. Returning true means the backend will apply what
    // it can and report anything it could not through Outcome.
    virtual bool acceptGuidance(const GuidanceField& field, std::string& reason) {
        (void)field;
        reason = name() + " does not implement flow guides or painted density";
        return false;
    }

    // Guidance that was ACCEPTED up front but could not be honored during the
    // last quadrangulate() call (e.g. the island fell through to a solve with
    // no guide hook). Queried by the pipeline after every run and surfaced in
    // the per-island report — never swallowed. Empty by default.
    [[nodiscard]] virtual std::vector<std::string> unhonoredGuidance() const { return {}; }

    [[nodiscard]] virtual std::string name() const = 0;
};

// Greedy pairing: scores every interior non-feature edge shared by two
// triangles by the quality of the quad that merging them would produce
// (planarity x corner-angle squareness), then merges best-first. Leftover
// triangles stay triangles (quad-dominant output).
std::unique_ptr<IQuadrangulator> makeGreedyPairingQuadrangulator();

}  // namespace cyber::remesh
