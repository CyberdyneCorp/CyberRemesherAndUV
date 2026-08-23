// FBX import (mesh-io spec: "Import formats", "Export formats").
//
// The fixtures under tests/data are committed Blender exports — FBX has no
// permissively licensed writer, so the suite cannot author its own input.
// tests/data/generate_fbx_fixtures.py regenerates them.
#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "cyber/core/io.hpp"
#include "cyber/core/mesh.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
namespace io = cyber::io;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) { return fs::path(CYBER_TEST_DATA_DIR) / name; }

std::size_t countFacesWithArity(const Mesh& mesh, std::size_t arity) {
    std::size_t n = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (mesh.isAlive(f) && mesh.faceLoops(f).size() == arity) {
            ++n;
        }
    }
    return n;
}

void checkNear(Vec3 a, Vec3 b, float tolerance = 1e-4f) {
    CHECK(std::abs(a.x - b.x) < tolerance);
    CHECK(std::abs(a.y - b.y) < tolerance);
    CHECK(std::abs(a.z - b.z) < tolerance);
}

}  // namespace

TEST_CASE("FBX import keeps quad faces at their authored arity") {
    const auto result = io::importMesh(fixture("cube_quads.fbx"));
    REQUIRE(result.ok());
    const Mesh& mesh = result.value().mesh;

    CHECK(mesh.vertexCount() == 8);
    CHECK(mesh.faceCount() == 6);
    CHECK(countFacesWithArity(mesh, 4) == 6);
}

TEST_CASE("FBX import reads UVs, normals and vertex colors") {
    const auto result = io::importMesh(fixture("cube_quads.fbx"));
    REQUIRE(result.ok());
    const Mesh& mesh = result.value().mesh;

    REQUIRE(mesh.cornerAttributes().find<Vec2>(io::kUvAttribute) != nullptr);
    REQUIRE(mesh.cornerAttributes().find<Vec3>(io::kNormalAttribute) != nullptr);
    REQUIRE(mesh.vertexAttributes().find<Vec3>(io::kColorAttribute) != nullptr);

    // Cube normals are axis-aligned unit vectors whichever way the exporter
    // ordered them, so length is the invariant worth asserting.
    const auto& normals = *mesh.cornerAttributes().find<Vec3>(io::kNormalAttribute);
    for (const Vec3& n : normals) {
        CHECK(std::abs(std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z) - 1.0f) < 1e-3f);
    }
}

TEST_CASE("FBX import triangulates when the caller asks for it") {
    io::ImportOptions options;
    options.polygons = io::PolygonPolicy::Triangulate;
    const auto result = io::importMesh(fixture("cube_quads.fbx"), options);
    REQUIRE(result.ok());
    const Mesh& mesh = result.value().mesh;

    CHECK(mesh.faceCount() == 12);
    CHECK(countFacesWithArity(mesh, 4) == 0);
}

TEST_CASE("FBX import normalizes axis and unit conventions") {
    // The same physical 2 m cube described three ways: Y-up, Z-up, and authored
    // in a centimetre scene (200 units across). All three must land on the same
    // 2-unit bounding box in the engine frame.
    const auto yUp = io::importMesh(fixture("cube_quads.fbx"));
    const auto zUp = io::importMesh(fixture("cube_zup.fbx"));
    const auto cm = io::importMesh(fixture("cube_cm.fbx"));
    REQUIRE(yUp.ok());
    REQUIRE(zUp.ok());
    REQUIRE(cm.ok());

    checkNear(yUp.value().boundsMin, zUp.value().boundsMin);
    checkNear(yUp.value().boundsMax, zUp.value().boundsMax);
    checkNear(yUp.value().boundsMin, cm.value().boundsMin);
    checkNear(yUp.value().boundsMax, cm.value().boundsMax);

    // FBX splits scale between geometry and the node instancing it: Blender
    // writes this cube as +/-1 geometry under a node scaled by 100 and declares
    // one unit to be a centimetre. Ignoring either half gives a 200-unit cube.
    CHECK(std::abs(yUp.value().boundsMax.x - yUp.value().boundsMin.x - 2.0f) < 1e-4f);
}

TEST_CASE("FBX import places every mesh node by its world transform") {
    const auto result = io::importMesh(fixture("two_cubes.fbx"));
    REQUIRE(result.ok());
    const Mesh& mesh = result.value().mesh;

    CHECK(mesh.vertexCount() == 16);
    CHECK(mesh.faceCount() == 12);
    // Cubes sit at x = -3 and x = +3, each 2 wide: merging them at the origin
    // (the bug this guards) would give a span of 2 rather than 8.
    CHECK(std::abs(result.value().boundsMax.x - result.value().boundsMin.x - 8.0f) < 1e-4f);
}

TEST_CASE("A corrupt FBX is rejected with a parse error") {
    const fs::path path = fs::temp_directory_path() / "cyber_broken.fbx";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "Kaydara FBX Binary  " << '\0' << "this is not a valid FBX body";
    }
    const auto result = io::importMesh(path);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::ParseError);
    CHECK(result.error().message.find(path.string()) != std::string::npos);
    fs::remove(path);
}

TEST_CASE("FBX export is refused with an error naming the writable formats") {
    const auto imported = io::importMesh(fixture("cube_quads.fbx"));
    REQUIRE(imported.ok());

    const fs::path path = fs::temp_directory_path() / "cyber_never_written.fbx";
    fs::remove(path);
    const io::Status status = io::exportMesh(imported.value().mesh, path);

    REQUIRE(!status.ok());
    CHECK(status.error().code == io::ErrorCode::UnsupportedFormat);
    CHECK(status.error().message.find("import-only") != std::string::npos);
    CHECK(status.error().message.find(".obj") != std::string::npos);
    CHECK(!fs::exists(path));
}
