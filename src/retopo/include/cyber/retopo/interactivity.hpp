#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/retopo/stroke_recognizer.hpp"

// Interactivity feedback and budgeting (manual-retopology spec, roadmap 9.5):
// a non-intrusive unrecognized-stroke feedback type, a recorded-trace fixture
// type for recognizer regression tables, and a lightweight operations-per-stroke
// cost estimate. Header-only and inline.
//
// TODO (documented, not implemented here): the concrete <=33 ms interaction
// budget at 5M triangles is a hardware benchmark that needs a GPU timing harness
// on real device targets; estimateCost() below only predicts the logical mesh
// work so a UI can pre-flight an interaction.
namespace cyber::retopo {

// Whether the recognizer understood the stroke.
enum class FeedbackKind { Recognized, Unrecognized };

// Non-intrusive feedback wrapping a StrokeResult: a UI can show the hint when a
// stroke is unrecognized instead of committing a wrong edit.
struct StrokeFeedback {
    StrokeAction action = StrokeAction::None;
    FeedbackKind kind = FeedbackKind::Unrecognized;
    float confidence = 0.0f;
    const char* hint = "";
};

// A stroke is "unrecognized" when it maps to no action or when the recognizer's
// confidence is below `minConfidence`.
[[nodiscard]] inline StrokeFeedback strokeFeedback(std::span<const Vec3> points,
                                                   float minConfidence = 0.5f,
                                                   const StrokeParams& params = {}) {
    const StrokeResult r = recognizeStroke(points, params);
    StrokeFeedback fb;
    fb.action = r.action;
    fb.confidence = r.confidence;
    if (r.action == StrokeAction::None || r.confidence < minConfidence) {
        fb.kind = FeedbackKind::Unrecognized;
        fb.hint = "stroke unclear - try a clearer closed shape, straight line, or drag";
    } else {
        fb.kind = FeedbackKind::Recognized;
        fb.hint = "";
    }
    return fb;
}

// A recorded stroke paired with the action it should classify as. Recognizer
// regression tests build a table of these and assert recognizeStroke matches.
struct RecordedTrace {
    std::vector<Vec3> points;
    StrokeAction expected = StrokeAction::None;
};

// Predicted mesh work for one recognized stroke (operations-per-stroke). Used to
// pre-flight an interaction against a budget without touching the mesh.
struct InteractivityCost {
    StrokeAction action = StrokeAction::None;
    std::size_t meshOps = 0;      // structural operators invoked
    std::size_t newVertices = 0;  // vertices created
    std::size_t newFaces = 0;     // faces created
};

// Lightweight, allocation-free cost estimate. `strokePoints` sizes the
// data-dependent actions (a cylinder extrude emits one quad per ring segment).
[[nodiscard]] inline InteractivityCost estimateCost(const StrokeResult& result,
                                                    std::size_t strokePoints) {
    InteractivityCost c;
    c.action = result.action;
    switch (result.action) {
        case StrokeAction::CreateQuad:
            c.newVertices = 4u;
            c.newFaces = 1u;
            c.meshOps = 5u;  // 4 addVertex + 1 addFace
            break;
        case StrokeAction::CreateTri:
            c.newVertices = 3u;
            c.newFaces = 1u;
            c.meshOps = 4u;  // 3 addVertex + 1 addFace
            break;
        case StrokeAction::InsertLoop:
            c.newVertices = 2u;
            c.newFaces = 1u;
            c.meshOps = 3u;  // 2 splitEdge + 1 splitFace
            break;
        case StrokeAction::Delete:
            c.meshOps = 1u;  // removeFace
            break;
        case StrokeAction::Merge:
            c.meshOps = 1u;  // collapseEdge
            break;
        case StrokeAction::RotateEdge:
            c.meshOps = 1u;  // flipEdge
            break;
        case StrokeAction::ExtrudeCylinder:
            c.newVertices = strokePoints;             // one lifted ring
            c.newFaces = strokePoints;                // one quad per segment
            c.meshOps = strokePoints + strokePoints;  // addVertex + addFace per segment
            break;
        case StrokeAction::Tweak:
            c.meshOps = 1u;  // setPosition
            break;
        case StrokeAction::None:
        default:
            break;
    }
    return c;
}

}  // namespace cyber::retopo
