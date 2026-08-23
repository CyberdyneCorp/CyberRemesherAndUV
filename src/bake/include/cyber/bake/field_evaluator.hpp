#pragma once

#include <cmath>

#include "cyber/core/math.hpp"

// Field-sampled baking (pipeline-bridge spec, "Field-sampled baking through an
// evaluator interface").
//
// This interface is the ONLY coupling point between baking and a volumetric
// engine. It is header-only and depends on nothing beyond cyber/core/math.hpp,
// so a build with no volumetric engine anywhere in it is completely unaffected;
// the engine never links a specific sculpting or SDF library.
namespace cyber::bake {

// A sampleable scalar field over world space. Implementations are typically a
// signed distance field: negative inside, positive outside, zero on the
// surface.
//
// CONTRACT (sphere tracing depends on it): distance() must be a Lipschitz-<=1
// LOWER BOUND on the true distance to the surface — i.e. it may under-estimate
// but must never over-estimate. An evaluator that returns a loose (too large)
// bound will make the tracer overshoot and miss thin features. This cannot be
// enforced here; it is the implementer's obligation.
class FieldEvaluator {
public:
    virtual ~FieldEvaluator() = default;

    // Signed distance from `p` to the surface. Negative inside.
    [[nodiscard]] virtual float distance(Vec3 p) const = 0;

    // Field gradient at `p`. Need not be unit length; the bake normalizes it.
    // On a signed distance field this is the outward surface normal.
    [[nodiscard]] virtual Vec3 gradient(Vec3 p) const = 0;

    // Openness at `p` with outward normal `n` over a hemisphere of `radius`:
    // 1 = fully open, 0 = fully occluded. Evaluators that already maintain an
    // occlusion cache answer this far more cheaply than any ray budget.
    [[nodiscard]] virtual float occlusion(Vec3 p, Vec3 n, float radius) const = 0;

    // Mean curvature at `p`: half the divergence of the normalized gradient,
    // by central differences over a step `h`. Non-pure so implementers get a
    // correct default for free; override it when the field knows better.
    //
    // Sign and units match cyber::bake::vertexMeanCurvature — 1/length, convex
    // POSITIVE, concave negative, so a sphere of radius r reads 1/r.
    [[nodiscard]] virtual float curvature(Vec3 p, float h) const {
        if (!(h > 0.0f)) {
            return 0.0f;
        }
        const Vec3 dx{h, 0.0f, 0.0f};
        const Vec3 dy{0.0f, h, 0.0f};
        const Vec3 dz{0.0f, 0.0f, h};
        const float spread = (normalized(gradient(p + dx)).x - normalized(gradient(p - dx)).x) +
                             (normalized(gradient(p + dy)).y - normalized(gradient(p - dy)).y) +
                             (normalized(gradient(p + dz)).z - normalized(gradient(p - dz)).z);
        return spread / (4.0f * h);  // 0.5 * (spread / 2h)
    }
};

}  // namespace cyber::bake
