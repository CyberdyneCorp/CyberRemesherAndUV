// C ABI cover for the isotropic (triangle) remesh entry point.
//
// The engine's isotropicRemesh had no C entry point at all, so no binding
// consumer could densify or re-tessellate a triangle mesh — the single most
// asked-for "give me more polygons to work with" operation. These cases drive
// cyber_mesh_isotropic_remesh through the pure-C surface and assert the two
// things a caller actually buys: that targetEdgeLength really controls output
// density, and that adaptivity really reaches the engine's scale field rather
// than being dropped on the way in (the failure mode cyber_remesh's
// sharpEdgeDegrees regression already had once).
#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "cyber_capi.h"

namespace {

// UV-sphere OBJ text with independent radii per axis. A plain sphere has
// uniform curvature, so the adaptive scale field normalizes to 1 everywhere
// and adaptivity is a no-op on it by design — an ellipsoid squashed along z
// has genuinely varying curvature (tight poles, flat flanks) and is therefore
// the shape that can tell the two settings apart. Quad body with triangle fans
// at the poles: the topology every DCC hands a binding, and the reason the
// entry point has to triangulate for itself.
std::string ellipsoidObj(float rx, float ry, float rz, int rings, int segments) {
    constexpr float kPi = 3.14159265358979323846f;
    const auto ring = [segments](int r, int s) {
        return 2 + static_cast<std::uint32_t>((r - 1) * segments +
                                              ((s % segments) + segments) % segments);
    };
    const std::uint32_t south = ring(rings - 1, 0) + static_cast<std::uint32_t>(segments);

    std::string obj = "v 0 0 " + std::to_string(rz) + "\n";  // north pole is vertex 1
    for (int r = 1; r < rings; ++r) {
        const float phi = kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float theta = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            obj += "v " + std::to_string(rx * std::sin(phi) * std::cos(theta)) + " " +
                   std::to_string(ry * std::sin(phi) * std::sin(theta)) + " " +
                   std::to_string(rz * std::cos(phi)) + "\n";
        }
    }
    obj += "v 0 0 " + std::to_string(-rz) + "\n";

    for (int s = 0; s < segments; ++s) {
        obj += "f 1 " + std::to_string(ring(1, s)) + " " + std::to_string(ring(1, s + 1)) + "\n";
    }
    for (int r = 1; r + 1 < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            obj += "f " + std::to_string(ring(r, s)) + " " + std::to_string(ring(r + 1, s)) + " " +
                   std::to_string(ring(r + 1, s + 1)) + " " + std::to_string(ring(r, s + 1)) + "\n";
        }
    }
    for (int s = 0; s < segments; ++s) {
        obj += "f " + std::to_string(south) + " " + std::to_string(ring(rings - 1, s + 1)) + " " +
               std::to_string(ring(rings - 1, s)) + "\n";
    }
    return obj;
}

// Loads OBJ text through the C loader, so every case starts from the same
// pure-C path a binding consumer would take.
CyberMesh* loadObjText(const std::string& text, const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    {
        std::ofstream out(path);
        out << text;
    }
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(path.string().c_str(), &mesh) == CYBER_OK);
    REQUIRE(mesh != nullptr);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return mesh;
}

struct EdgeSpread {
    float minLength = 0.0f;
    float maxLength = 0.0f;
    float meanLength = 0.0f;
    std::size_t count = 0;
};

// Edge lengths straight off the ABI's own wireframe stream: the edge index
// buffer and the render positions it indexes are the pair a host renderer
// uses, so this measures what a caller can actually see.
EdgeSpread edgeSpread(const CyberMesh* mesh) {
    std::vector<std::uint32_t> edges(cyber_mesh_copy_edge_indices(mesh, nullptr, 0));
    cyber_mesh_copy_edge_indices(mesh, edges.data(), edges.size());
    std::vector<float> positions(cyber_mesh_copy_render_positions(mesh, nullptr, 0));
    cyber_mesh_copy_render_positions(mesh, positions.data(), positions.size());

    EdgeSpread spread;
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < edges.size(); i += 2) {
        const std::size_t a = 3 * static_cast<std::size_t>(edges[i]);
        const std::size_t b = 3 * static_cast<std::size_t>(edges[i + 1]);
        REQUIRE(a + 2 < positions.size());
        REQUIRE(b + 2 < positions.size());
        const float dx = positions[a] - positions[b];
        const float dy = positions[a + 1] - positions[b + 1];
        const float dz = positions[a + 2] - positions[b + 2];
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        spread.minLength = spread.count == 0 ? len : std::fmin(spread.minLength, len);
        spread.maxLength = std::fmax(spread.maxLength, len);
        total += static_cast<double>(len);
        ++spread.count;
    }
    spread.meanLength =
        spread.count == 0 ? 0.0f : static_cast<float>(total / static_cast<double>(spread.count));
    return spread;
}

std::vector<float> positionsOf(const CyberMesh* mesh) {
    std::vector<float> positions(cyber_mesh_copy_positions(mesh, nullptr, 0));
    cyber_mesh_copy_positions(mesh, positions.data(), positions.size());
    return positions;
}

}  // namespace

TEST_CASE("capi isotropic default params leave the caller to pick the edge length") {
    CyberIsotropicParams params{};
    cyber_default_isotropic_params(&params);
    // The filler answers everything a scale-free default can answer, and
    // deliberately does NOT invent a world-space length.
    CHECK(params.targetEdgeLength == 0.0f);
    CHECK(params.iterations >= 1);
    CHECK(params.adaptivity == 0.0f);
    CHECK(params.sharpEdgeDegrees > 0.0f);
    cyber_default_isotropic_params(nullptr);  // tolerated
}

TEST_CASE("capi isotropic remesh: target edge length controls output density") {
    const std::string obj = ellipsoidObj(1.0f, 1.0f, 1.0f, 12, 18);

    const auto remeshAt = [&obj](float target) {
        CyberMesh* mesh = loadObjText(obj, "cyber_capi_iso_density.obj");
        CyberIsotropicParams params{};
        cyber_default_isotropic_params(&params);
        params.targetEdgeLength = target;
        std::size_t faces = 0;
        REQUIRE(cyber_mesh_isotropic_remesh(mesh, &params, &faces) == CYBER_OK);
        // The error slot is the ABI's definition of success and must be empty.
        CHECK(std::string(cyber_last_error()).empty());
        CHECK(faces == cyber_mesh_face_count(mesh));
        const EdgeSpread spread = edgeSpread(mesh);
        cyber_mesh_free(mesh);
        return std::pair{faces, spread};
    };

    const auto [coarseFaces, coarseSpread] = remeshAt(0.40f);
    const auto [fineFaces, fineSpread] = remeshAt(0.15f);

    // Density is what this entry point exists to give a caller: an edge length
    // ~2.7x smaller must produce many more faces, not a rounding difference.
    CHECK(fineFaces > 4 * coarseFaces);
    // And the mean edge length has to actually track the request, or the knob
    // is only changing the count by accident.
    CHECK(coarseSpread.meanLength > 2.0f * fineSpread.meanLength);
    CHECK(fineSpread.meanLength < 0.40f * 4.0f / 3.0f);
    CHECK(coarseSpread.meanLength > 0.15f);
}

TEST_CASE("capi isotropic remesh: adaptivity reaches the engine's scale field") {
    // Squashed along z: the poles carry most of the curvature and the flanks
    // are comparatively flat, which is exactly what adaptivity redistributes.
    const std::string obj = ellipsoidObj(1.0f, 1.0f, 0.35f, 14, 22);

    const auto remeshAt = [&obj](float adaptivity) {
        CyberMesh* mesh = loadObjText(obj, "cyber_capi_iso_adaptive.obj");
        CyberIsotropicParams params{};
        cyber_default_isotropic_params(&params);
        params.targetEdgeLength = 0.12f;
        params.adaptivity = adaptivity;
        REQUIRE(cyber_mesh_isotropic_remesh(mesh, &params, nullptr) == CYBER_OK);
        const EdgeSpread spread = edgeSpread(mesh);
        const std::vector<float> positions = positionsOf(mesh);
        cyber_mesh_free(mesh);
        return std::pair{spread, positions};
    };

    const auto [uniform, uniformPositions] = remeshAt(0.0f);
    const auto [adaptive, adaptivePositions] = remeshAt(1.0f);

    // Two runs differing only in the knob cannot agree unless the value is
    // being dropped on the way in — the failure the sharpEdgeDegrees
    // regression above documents for the pipeline entry point.
    CHECK(uniformPositions != adaptivePositions);
    // The substantive claim: adaptivity spends edges where curvature is,
    // which widens the length distribution. A uniform run converges every
    // edge into the [4/5, 4/3] band around one target.
    REQUIRE(uniform.count > 100);
    REQUIRE(adaptive.count > 100);
    CHECK(adaptive.maxLength / adaptive.minLength > uniform.maxLength / uniform.minLength);

    // Same value twice is still deterministic: the spread is the knob, not
    // run-to-run noise.
    CHECK(remeshAt(1.0f).second == adaptivePositions);
}

TEST_CASE("capi isotropic remesh triangulates a quad mesh instead of rejecting it") {
    // The header's documented answer to non-triangulated input. A caller's
    // most obvious move is to densify the quads cyber_remesh just produced, so
    // this path must work in one call.
    CyberMesh* mesh =
        loadObjText(ellipsoidObj(1.0f, 1.0f, 1.0f, 10, 14), "cyber_capi_iso_quads.obj");
    CyberStats before{};
    REQUIRE(cyber_mesh_stats(mesh, &before) == CYBER_OK);
    REQUIRE(before.quads > 0);

    CyberIsotropicParams params{};
    cyber_default_isotropic_params(&params);
    params.targetEdgeLength = 0.25f;
    REQUIRE(cyber_mesh_isotropic_remesh(mesh, &params, nullptr) == CYBER_OK);

    CyberStats after{};
    REQUIRE(cyber_mesh_stats(mesh, &after) == CYBER_OK);
    CHECK(after.triangles > 0);
    CHECK(after.quads == 0);
    CHECK(after.other == 0);
    cyber_mesh_free(mesh);
}

TEST_CASE("capi isotropic remesh rejects unusable arguments without touching the mesh") {
    CyberMesh* mesh =
        loadObjText(ellipsoidObj(1.0f, 1.0f, 1.0f, 8, 10), "cyber_capi_iso_reject.obj");
    const std::vector<float> before = positionsOf(mesh);

    CyberIsotropicParams params{};
    cyber_default_isotropic_params(&params);

    CHECK(cyber_mesh_isotropic_remesh(nullptr, &params, nullptr) == CYBER_ERR_INVALID_ARG);
    CHECK(std::string(cyber_last_error()).find("mesh") != std::string::npos);

    CHECK(cyber_mesh_isotropic_remesh(mesh, nullptr, nullptr) == CYBER_ERR_INVALID_ARG);
    CHECK(std::string(cyber_last_error()).find("params") != std::string::npos);

    // The defaults filler leaves targetEdgeLength at 0, which is refused
    // rather than silently turned into some invented length.
    CHECK(cyber_mesh_isotropic_remesh(mesh, &params, nullptr) == CYBER_ERR_INVALID_PARAM);
    CHECK(std::string(cyber_last_error()).find("targetEdgeLength") != std::string::npos);

    params.targetEdgeLength = std::numeric_limits<float>::quiet_NaN();
    CHECK(cyber_mesh_isotropic_remesh(mesh, &params, nullptr) == CYBER_ERR_INVALID_PARAM);

    params.targetEdgeLength = 0.3f;
    params.iterations = 0;
    CHECK(cyber_mesh_isotropic_remesh(mesh, &params, nullptr) == CYBER_ERR_INVALID_PARAM);
    CHECK(std::string(cyber_last_error()).find("iterations") != std::string::npos);

    // Every one of those is decided before the mesh is touched.
    CHECK(positionsOf(mesh) == before);

    CyberMesh* empty = cyber_mesh_create();
    REQUIRE(empty != nullptr);
    CHECK(cyber_mesh_isotropic_remesh(empty, &params, nullptr) == CYBER_ERR_EMPTY);
    cyber_mesh_destroy(empty);

    cyber_mesh_free(mesh);
}
