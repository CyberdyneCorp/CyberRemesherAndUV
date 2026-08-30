#include <doctest.h>

#include <cmath>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/quadrangulate/geometry_analysis.hpp"
#include "cyber/quadrangulate/sizing_field.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec3;
using cyber::VertexId;
using cyber::remesh::analyzeGeometry;
using cyber::remesh::buildSizingField;
using cyber::remesh::GeometryAnalysis;
using cyber::remesh::SizingField;
using cyber::remesh::SizingParams;

namespace {

Mesh flatGrid(int n) {
    std::vector<Vec3> p;
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            p.push_back(Vec3{static_cast<float>(x), static_cast<float>(y), 0.0f});
        }
    }
    const auto at = [n](int x, int y) { return static_cast<Index>(y * (n + 1) + x); };
    std::vector<std::vector<Index>> f;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            f.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    return Mesh::fromIndexed(p, f);
}

// A GeometryAnalysis with every field zero: the neutral input.
GeometryAnalysis flatAnalysis(std::size_t vertices) {
    GeometryAnalysis g;
    g.valid = true;
    g.normalizedCurvature.assign(vertices, 0.0f);
    g.featureInfluence.assign(vertices, 0.0f);
    g.boundaryInfluence.assign(vertices, 0.0f);
    g.featureCorner.assign(vertices, 0.0f);
    g.thinFeatureRisk.assign(vertices, 0.0f);
    return g;
}

}  // namespace

TEST_CASE("sizing field: neutral input is exactly uniform") {
    // The property that lets the field be introduced without changing any
    // existing output: nothing to adapt to means nothing adapts.
    const Mesh mesh = flatGrid(4);
    const GeometryAnalysis g = flatAnalysis(mesh.vertexCapacity());
    SizingParams params;
    params.baseEdgeLength = 0.7f;
    const SizingField field = buildSizingField(mesh, nullptr, g, params);
    REQUIRE(field.valid);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (mesh.isAlive(VertexId{v})) {
            CHECK(field.at(VertexId{v}) == doctest::Approx(0.7f));
        }
    }
    CHECK(field.minLength() == doctest::Approx(field.maxLength()));
}

TEST_CASE("sizing field: adaptivity 0 ignores curvature entirely") {
    // Adaptivity is the pipeline's existing uniform-vs-adaptive switch, so 0
    // must mean EXACTLY uniform rather than "weakly curved" — otherwise turning
    // adaptivity off would still move the output.
    const Mesh mesh = flatGrid(4);
    GeometryAnalysis g = flatAnalysis(mesh.vertexCapacity());
    for (float& c : g.normalizedCurvature) {
        c = 1.0f;  // maximally curved everywhere
    }
    SizingParams params;
    params.baseEdgeLength = 1.0f;
    params.adaptivity = 0.0f;
    const SizingField field = buildSizingField(mesh, nullptr, g, params);
    REQUIRE(field.valid);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (mesh.isAlive(VertexId{v})) {
            CHECK(field.at(VertexId{v}) == doctest::Approx(1.0f));
        }
    }
}

TEST_CASE("sizing field: curvature shortens edges once adaptivity is on") {
    const Mesh mesh = flatGrid(4);
    GeometryAnalysis g = flatAnalysis(mesh.vertexCapacity());
    for (float& c : g.normalizedCurvature) {
        c = 1.0f;
    }
    SizingParams params;
    params.baseEdgeLength = 1.0f;
    params.adaptivity = 1.0f;
    params.smoothingPasses = 0;
    const SizingField field = buildSizingField(mesh, nullptr, g, params);
    REQUIRE(field.valid);
    CHECK(field.maxLength() < 1.0f);
    CHECK(field.minLength() >= params.minScale);
}

TEST_CASE("sizing field: every term is separately switchable off") {
    // Each weight at 0 removes exactly its own term, which is what makes a
    // regression traceable to one input rather than to "the sizing field".
    const Mesh mesh = flatGrid(3);
    GeometryAnalysis g = flatAnalysis(mesh.vertexCapacity());
    for (std::size_t i = 0; i < g.featureInfluence.size(); ++i) {
        g.featureInfluence[i] = 1.0f;
        g.thinFeatureRisk[i] = 1.0f;
    }
    SizingParams params;
    params.baseEdgeLength = 1.0f;
    params.smoothingPasses = 0;

    const SizingField both = buildSizingField(mesh, nullptr, g, params);
    CHECK(both.maxLength() < 1.0f);

    params.featureWeight = 0.0f;
    params.thinFeatureWeight = 0.0f;
    const SizingField neither = buildSizingField(mesh, nullptr, g, params);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (mesh.isAlive(VertexId{v})) {
            CHECK(neither.at(VertexId{v}) == doctest::Approx(1.0f));
        }
    }
}

TEST_CASE("sizing field: the scale bounds are a hard contract") {
    // A term that could drive the target toward zero would produce an unbounded
    // quad count, so the clamp has to survive smoothing too.
    const Mesh mesh = flatGrid(4);
    GeometryAnalysis g = flatAnalysis(mesh.vertexCapacity());
    for (std::size_t i = 0; i < g.thinFeatureRisk.size(); ++i) {
        g.thinFeatureRisk[i] = 1.0f;
        g.featureInfluence[i] = 1.0f;
        g.normalizedCurvature[i] = 1.0f;
    }
    SizingParams params;
    params.baseEdgeLength = 2.0f;
    params.adaptivity = 1.0f;
    params.minScale = 0.5f;
    params.maxScale = 1.5f;
    params.thinFeatureWeight = 100.0f;  // absurd on purpose
    const SizingField field = buildSizingField(mesh, nullptr, g, params);
    REQUIRE(field.valid);
    CHECK(field.minLength() >= doctest::Approx(2.0f * 0.5f));
    CHECK(field.maxLength() <= doctest::Approx(2.0f * 1.5f));
}

TEST_CASE("sizing field: smoothing narrows the field without biasing it coarse") {
    // Target edge length is MULTIPLICATIVE, so smoothing happens in log space.
    // Smoothing it arithmetically would pull every mixed neighbourhood toward
    // the arithmetic mean, which for h/2 and 2h is 1.25h rather than h — a
    // systematic coarsening bias that silently loses detail wherever the field
    // varies. In log space the same pair averages to h.
    //
    // The observable consequences, both asserted here: smoothing narrows the
    // spread, and it obeys a maximum principle — no smoothed value escapes the
    // range of the unsmoothed one, so smoothing cannot invent a coarser target
    // than any input asked for.
    const Mesh mesh = flatGrid(6);
    GeometryAnalysis g = flatAnalysis(mesh.vertexCapacity());
    // Curvature on one half only, so the field genuinely varies.
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        if (mesh.isAlive(VertexId{v}) && mesh.position(VertexId{v}).x < 3.0f) {
            g.normalizedCurvature[v] = 1.0f;
        }
    }
    SizingParams params;
    params.baseEdgeLength = 1.0f;
    params.adaptivity = 1.0f;

    params.smoothingPasses = 0;
    const SizingField raw = buildSizingField(mesh, nullptr, g, params);
    params.smoothingPasses = 12;
    const SizingField smooth = buildSizingField(mesh, nullptr, g, params);
    REQUIRE(raw.valid);
    REQUIRE(smooth.valid);

    // It actually varied, or the test proves nothing.
    REQUIRE(raw.maxLength() > raw.minLength() * 1.1f);
    // Narrower...
    CHECK(smooth.maxLength() - smooth.minLength() < raw.maxLength() - raw.minLength());
    // ...and strictly inside the original range.
    CHECK(smooth.maxLength() <= doctest::Approx(raw.maxLength()));
    CHECK(smooth.minLength() >= doctest::Approx(raw.minLength()));
}

TEST_CASE("sizing field: face lookup averages its corners") {
    const Mesh mesh = flatGrid(2);
    GeometryAnalysis g = flatAnalysis(mesh.vertexCapacity());
    SizingParams params;
    params.baseEdgeLength = 1.5f;
    const SizingField field = buildSizingField(mesh, nullptr, g, params);
    REQUIRE(field.valid);
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        if (mesh.isAlive(FaceId{f})) {
            CHECK(field.at(FaceId{f}, mesh) == doctest::Approx(1.5f));
        }
    }
}

TEST_CASE("sizing field: an unbuilt field answers with the base length") {
    // Consumers are written once and run with or without a field; an unbuilt
    // one has to behave as "uniform", never as zero.
    const Mesh mesh = flatGrid(2);
    const SizingField none;
    CHECK(none.at(VertexId{0}) == doctest::Approx(1.0f));
    CHECK(none.at(FaceId{0}, mesh) == doctest::Approx(1.0f));
    CHECK(none.minLength() == doctest::Approx(none.maxLength()));
}

TEST_CASE("sizing field is deterministic") {
    const Mesh mesh = flatGrid(5);
    const GeometryAnalysis g = analyzeGeometry(mesh, 1.0f);
    SizingParams params;
    params.adaptivity = 1.0f;
    const SizingField first = buildSizingField(mesh, nullptr, g, params);
    for (int i = 0; i < 4; ++i) {
        const SizingField again = buildSizingField(mesh, nullptr, g, params);
        CHECK(again.targetEdgeLength == first.targetEdgeLength);
    }
}
