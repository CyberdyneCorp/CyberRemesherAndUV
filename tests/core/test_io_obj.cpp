#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cyber/core/io.hpp"
#include "cyber/core/mesh.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
using cyber::VertexId;
namespace io = cyber::io;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    const fs::path dir = fs::temp_directory_path() / "cyber_io_tests";
    fs::create_directories(dir);
    return dir;
}

fs::path writeFile(const std::string& name, const std::string& content) {
    const fs::path p = tempDir() / name;
    std::ofstream f(p, std::ios::trunc);
    f << content;
    return p;
}

}  // namespace

TEST_CASE("quad OBJ imports with correct arity (spec: mesh-io)") {
    // AutoRemesher silently mis-parsed exactly this file shape.
    const auto path = writeFile("quads.obj",
                                "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                "v 2 0 0\nv 2 1 0\n"
                                "f 1 2 3 4\nf 2 5 6 3\n");
    auto result = io::importMesh(path);
    REQUIRE(result.ok());
    Mesh& mesh = result.value().mesh;
    REQUIRE(mesh.faceCount() == 2);
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        if (mesh.isAlive(FaceId{i})) {
            REQUIRE(mesh.faceSize(FaceId{i}) == 4);
        }
    }
    REQUIRE(mesh.validate().empty());
}

TEST_CASE("triangulate policy fans n-gons on import") {
    const auto path = writeFile("pentagon.obj",
                                "v 0 0 0\nv 1 0 0\nv 1.5 1 0\nv 0.5 1.8 0\nv -0.5 1 0\n"
                                "f 1 2 3 4 5\n");
    io::ImportOptions options;
    options.polygons = io::PolygonPolicy::Triangulate;
    auto result = io::importMesh(path, options);
    REQUIRE(result.ok());
    REQUIRE(result.value().mesh.faceCount() == 3);
}

TEST_CASE("vertex colors import from xyzrgb OBJ (spec: mesh-io polypaint)") {
    const auto path = writeFile("colored.obj",
                                "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\n"
                                "f 1 2 3\n");
    auto result = io::importMesh(path);
    REQUIRE(result.ok());
    const auto* colors = result.value().mesh.vertexAttributes().find<Vec3>(io::kColorAttribute);
    REQUIRE(colors != nullptr);
    REQUIRE((*colors)[0].x == doctest::Approx(1.0f));
    REQUIRE((*colors)[1].y == doctest::Approx(1.0f));
    REQUIRE((*colors)[2].z == doctest::Approx(1.0f));
}

TEST_CASE("uncolored OBJ creates no color attribute") {
    const auto path = writeFile("plain.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    auto result = io::importMesh(path);
    REQUIRE(result.ok());
    REQUIRE(result.value().mesh.vertexAttributes().find<Vec3>(io::kColorAttribute) == nullptr);
}

TEST_CASE("missing file is a typed FileNotFound error (spec: mesh-io)") {
    auto result = io::importMesh(tempDir() / "does_not_exist.obj");
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::FileNotFound);
    REQUIRE(result.error().message.find("does_not_exist.obj") != std::string::npos);
}

TEST_CASE("unsupported extension is a typed error") {
    const auto path = writeFile("mesh.xyz", "nonsense");
    auto result = io::importMesh(path);
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::UnsupportedFormat);
}

TEST_CASE("file with no faces is a typed EmptyMesh error") {
    const auto path = writeFile("points.obj", "v 0 0 0\nv 1 0 0\n");
    auto result = io::importMesh(path);
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::EmptyMesh);
}

TEST_CASE("unwritable export path is a typed WriteFailed error (spec: mesh-io)") {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2}};
    const Mesh mesh = Mesh::fromIndexed(p, f);
    const auto status = io::exportMesh(mesh, "/nonexistent_dir_xyz/out.obj");
    REQUIRE(!status.ok());
    REQUIRE(status.error().code == io::ErrorCode::WriteFailed);
    REQUIRE(status.error().message.find("/nonexistent_dir_xyz/out.obj") != std::string::npos);
}

TEST_CASE("exporting an empty mesh is refused loudly") {
    const Mesh empty;
    const auto status = io::exportMesh(empty, tempDir() / "empty.obj");
    REQUIRE(!status.ok());
    REQUIRE(status.error().code == io::ErrorCode::EmptyMesh);
}

TEST_CASE("OBJ round-trip preserves quads, positions, colors and UVs") {
    const std::vector<Vec3> p = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& colors = mesh.vertexAttributes().create<Vec3>(io::kColorAttribute);
    colors = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}};
    auto& uvs = mesh.cornerAttributes().create<Vec2>(io::kUvAttribute);
    for (const cyber::LoopId l : mesh.faceLoops(FaceId{0})) {
        const Vec3 pos = mesh.position(mesh.loopVertex(l));
        uvs[l.value] = {pos.x / 2.0f, pos.y / 2.0f};
    }

    const fs::path path = tempDir() / "roundtrip.obj";
    REQUIRE(io::exportMesh(mesh, path).ok());

    auto result = io::importMesh(path);
    REQUIRE(result.ok());
    Mesh& back = result.value().mesh;
    REQUIRE(back.faceCount() == 1);
    REQUIRE(back.faceSize(FaceId{0}) == 4);
    REQUIRE(back.vertexCount() == 4);
    // Scale round-trip (spec: mesh-io "Import scale and unit sanity").
    REQUIRE(result.value().boundsMax.x == doctest::Approx(2.0f));
    const auto* backColors = back.vertexAttributes().find<Vec3>(io::kColorAttribute);
    REQUIRE(backColors != nullptr);
    REQUIRE((*backColors)[0].x == doctest::Approx(1.0f));
    const auto* backUvs = back.cornerAttributes().find<Vec2>(io::kUvAttribute);
    REQUIRE(backUvs != nullptr);
    // MTL sibling written and referenced.
    REQUIRE(fs::exists(tempDir() / "roundtrip.mtl"));
}

TEST_CASE("malformed OBJ face indices are skipped with a warning") {
    const auto path = writeFile("badface.obj",
                                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                "f 1 2 3\nf 1 2 99\n");
    auto result = io::importMesh(path);
    REQUIRE(result.ok());
    REQUIRE(result.value().mesh.faceCount() == 1);
    REQUIRE(!result.value().warnings.empty());
}
