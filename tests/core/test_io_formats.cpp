// Cross-format round-trip corpus tests (mesh-io spec: "Import formats",
// "Export formats"; tasks 3.2/3.5).
#include <doctest.h>

#include <filesystem>
#include <fstream>

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
    const fs::path dir = fs::temp_directory_path() / "cyber_io_format_tests";
    fs::create_directories(dir);
    return dir;
}

// Corpus builder: colored quad cube with UVs — exercises n-gons, vertex
// colors and corner attributes in one mesh.
Mesh makeCorpusCube() {
    const std::vector<Vec3> p = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    const std::vector<std::vector<Index>> f = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7},
    };
    Mesh mesh = Mesh::fromIndexed(p, f);
    auto& colors = mesh.vertexAttributes().create<Vec3>(io::kColorAttribute);
    for (Index i = 0; i < 8; ++i) {
        colors[i] = {static_cast<float>(i) / 8.0f, 0.5f, 1.0f - static_cast<float>(i) / 8.0f};
    }
    auto& uvs = mesh.cornerAttributes().create<Vec2>(io::kUvAttribute);
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        float u = 0.0f;
        for (const cyber::LoopId l : mesh.faceLoops(FaceId{fi})) {
            uvs[l.value] = {u, static_cast<float>(fi) / 6.0f};
            u += 0.25f;
        }
    }
    return mesh;
}

}  // namespace

TEST_CASE("PLY round-trip preserves quads and vertex colors") {
    const Mesh cube = makeCorpusCube();
    const fs::path path = tempDir() / "cube.ply";
    REQUIRE(io::exportMesh(cube, path).ok());

    auto result = io::importMesh(path);
    REQUIRE(result.ok());
    Mesh& back = result.value().mesh;
    REQUIRE(back.validate().empty());
    REQUIRE(back.vertexCount() == 8);
    REQUIRE(back.faceCount() == 6);
    for (Index i = 0; i < back.faceCapacity(); ++i) {
        if (back.isAlive(FaceId{i})) {
            REQUIRE(back.faceSize(FaceId{i}) == 4);  // quads preserved
        }
    }
    const auto* colors = back.vertexAttributes().find<Vec3>(io::kColorAttribute);
    REQUIRE(colors != nullptr);
    REQUIRE((*colors)[2].x == doctest::Approx(0.25f).epsilon(0.01));
}

TEST_CASE("STL round-trip welds shared vertices back together") {
    const Mesh cube = makeCorpusCube();
    const fs::path path = tempDir() / "cube.stl";
    REQUIRE(io::exportMesh(cube, path).ok());

    auto result = io::importMesh(path);
    REQUIRE(result.ok());
    Mesh& back = result.value().mesh;
    REQUIRE(back.validate().empty());
    REQUIRE(back.vertexCount() == 8);  // welded, not 36 loose corners
    REQUIRE(back.faceCount() == 12);   // triangulated by the format
    REQUIRE(result.value().boundsMax.x == doctest::Approx(1.0f));
}

TEST_CASE("ASCII STL imports") {
    const fs::path path = tempDir() / "tri.stl";
    std::ofstream f(path, std::ios::trunc);
    f << "solid tri\n"
         " facet normal 0 0 1\n  outer loop\n"
         "   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0\n"
         "  endloop\n endfacet\nendsolid tri\n";
    f.close();
    auto result = io::importMesh(path);
    REQUIRE(result.ok());
    REQUIRE(result.value().mesh.faceCount() == 1);
}

TEST_CASE("corrupt STL is a typed ParseError") {
    const fs::path path = tempDir() / "corrupt.stl";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << "garbage that is neither ascii nor a sane binary header";
    f.close();
    auto result = io::importMesh(path);
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("glTF round-trip preserves geometry, colors and UVs (.gltf and .glb)") {
    const Mesh cube = makeCorpusCube();
    for (const char* name : {"cube.gltf", "cube.glb"}) {
        CAPTURE(name);
        const fs::path path = tempDir() / name;
        REQUIRE(io::exportMesh(cube, path).ok());

        auto result = io::importMesh(path);
        REQUIRE(result.ok());
        Mesh& back = result.value().mesh;
        REQUIRE(back.validate().empty());
        REQUIRE(back.faceCount() == 12);  // triangles: glTF has no quads
        REQUIRE(result.value().boundsMax.z == doctest::Approx(1.0f));
        REQUIRE(back.vertexAttributes().find<Vec3>(io::kColorAttribute) != nullptr);
        REQUIRE(back.cornerAttributes().find<Vec2>(io::kUvAttribute) != nullptr);
    }
}

TEST_CASE("corrupt glTF is a typed ParseError (spec: mesh-io corrupt input)") {
    const fs::path path = tempDir() / "corrupt.gltf";
    std::ofstream f(path, std::ios::trunc);
    f << "{ not valid json ";
    f.close();
    auto result = io::importMesh(path);
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
    // Document unchanged is the caller's guarantee; importer returns no mesh.
}

TEST_CASE("corrupt PLY is a typed ParseError") {
    const fs::path path = tempDir() / "corrupt.ply";
    std::ofstream f(path, std::ios::trunc);
    f << "ply\nformat ascii 1.0\nelement vertex nonsense\n";
    f.close();
    auto result = io::importMesh(path);
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("cross-format pipeline: OBJ -> PLY -> STL keeps geometry") {
    const Mesh cube = makeCorpusCube();
    const fs::path obj = tempDir() / "chain.obj";
    REQUIRE(io::exportMesh(cube, obj).ok());
    auto fromObj = io::importMesh(obj);
    REQUIRE(fromObj.ok());

    const fs::path ply = tempDir() / "chain.ply";
    REQUIRE(io::exportMesh(fromObj.value().mesh, ply).ok());
    auto fromPly = io::importMesh(ply);
    REQUIRE(fromPly.ok());

    const fs::path stl = tempDir() / "chain.stl";
    REQUIRE(io::exportMesh(fromPly.value().mesh, stl).ok());
    auto fromStl = io::importMesh(stl);
    REQUIRE(fromStl.ok());
    REQUIRE(fromStl.value().mesh.vertexCount() == 8);
    REQUIRE(fromStl.value().boundsMax.y == doctest::Approx(1.0f));
}
