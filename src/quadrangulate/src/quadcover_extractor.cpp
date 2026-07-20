#include "cyber/quadrangulate/quadcover_extractor.hpp"

#include <memory>
#include <string>

// TASK F scaffold. See quadcover_extractor.hpp for the full rationale and the
// milestone plan. Every entry point here is a guarded stub: it compiles, links,
// and returns a well-defined "not implemented" result WITHOUT mutating the input
// mesh, so the pipeline degrades cleanly and the next engineer starts from green.
namespace cyber::remesh {

// Collaborator 1 stub: no seamless solver is vendored yet, so return an invalid
// SeamlessUv. Milestone 1 replaces this with a real solve (see the plan).
SeamlessUv computeSeamlessUv(const Mesh& /*mesh*/, float /*targetEdgeLength*/) {
    return SeamlessUv{};  // valid == false
}

// Collaborator 2 stub: with no seamless UV there are no isolines to trace, so
// return an empty mesh. Milestone 2 ports quadextractor.cpp here.
IsolineQuadMesh extractIsolineQuads(const Mesh& /*mesh*/, const SeamlessUv& /*uv*/) {
    return IsolineQuadMesh{};
}

namespace {

// IQuadrangulator implementation. The stub does not touch `mesh`: on the failure
// path the caller keeps its input triangle island unchanged, which is exactly the
// safe degrade behaviour the pipeline expects from a not-yet-implemented seam.
class QuadCoverQuadrangulator final : public IQuadrangulator {
public:
    explicit QuadCoverQuadrangulator(int fieldIterations) : m_fieldIterations(fieldIterations) {}

    Outcome quadrangulate(Mesh& mesh, float targetEdgeLength, ProgressSink* progress,
                          const CancelToken* cancel) override {
        if (cancel != nullptr && cancel->isCancelled()) {
            return {.success = false, .cancelled = true, .failureReason = {}};
        }
        if (progress != nullptr) {
            progress->report(0.0f, "quadrangulate (quad-cover: not implemented)");
        }

        // --- Real pipeline goes here (Milestones 1-4) ---
        //   SeamlessUv uv = computeSeamlessUv(mesh, targetEdgeLength);
        //   if (!uv.valid) return { .success = false, ... };
        //   IsolineQuadMesh out = extractIsolineQuads(mesh, uv);
        //   rewriteMeshInPlace(mesh, out);   // replace tris with quads
        // Until then, leave `mesh` untouched and report not-implemented.
        (void)mesh;
        (void)targetEdgeLength;
        (void)m_fieldIterations;

        return {.success = false,
                .cancelled = false,
                .failureReason = "quad-cover isoline extractor not implemented (Task F scaffold)"};
    }

    [[nodiscard]] std::string name() const override { return "quad-cover"; }

private:
    int m_fieldIterations;
};

}  // namespace

std::unique_ptr<IQuadrangulator> makeQuadCoverQuadrangulator(int fieldIterations) {
    return std::make_unique<QuadCoverQuadrangulator>(fieldIterations);
}

}  // namespace cyber::remesh
