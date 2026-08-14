// Cross-format round-trip corpus tests (mesh-io spec: "Import formats",
// "Export formats"; tasks 3.2/3.5).
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

// One-triangle glTF whose buffer is exactly 48 bytes: 3 VEC3 float positions
// followed by 3 uint32 indices. Malformed variants below are made by rewriting
// a single field, so the rest of the document stays valid and tinygltf parses.
namespace {

constexpr const char* kTriangleGltf = R"({
  "asset": {"version": "2.0"},
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "mode": 4}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 12}
  ],
  "buffers": [{"byteLength": 48, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAEAAAACAAAA"}]
})";

fs::path writeGltf(const char* name, const std::string& find, const std::string& replace) {
    std::string doc = kTriangleGltf;
    if (!find.empty()) {
        const std::size_t at = doc.find(find);
        REQUIRE(at != std::string::npos);
        doc.replace(at, find.size(), replace);
    }
    const fs::path path = tempDir() / name;
    std::ofstream f(path, std::ios::trunc);
    f << doc;
    return path;
}

}  // namespace

TEST_CASE("hand-written one-triangle glTF imports (control for the malformed cases)") {
    auto result = io::importMesh(writeGltf("triangle.gltf", "", ""));
    REQUIRE(result.ok());
    REQUIRE(result.value().mesh.faceCount() == 1);
    REQUIRE(result.value().mesh.vertexCount() == 3);
}

TEST_CASE("glTF with an out-of-range accessor index is a typed ParseError, not a crash") {
    // POSITION names accessor 99 against a two-accessor document.
    auto result =
        io::importMesh(writeGltf("bad_attr_index.gltf", R"("POSITION": 0)", R"("POSITION": 99)"));
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("glTF accessor with no bufferView is a typed ParseError, not a crash") {
    // tinygltf leaves the omitted "bufferView" at -1.
    auto result =
        io::importMesh(writeGltf("no_bufferview.gltf", R"({"bufferView": 0, "componentType": 5126)",
                                 R"({"componentType": 5126)"));
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("glTF bufferView pointing at a missing buffer is a typed ParseError, not a crash") {
    auto result =
        io::importMesh(writeGltf("bad_buffer_index.gltf", R"({"buffer": 0, "byteOffset": 0)",
                                 R"({"buffer": 7, "byteOffset": 0)"));
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("glTF attribute accessor over-declaring count is a typed ParseError, not an over-read") {
    // count claims 5M vertices behind a 36-byte bufferView.
    auto result =
        io::importMesh(writeGltf("huge_position_count.gltf", R"("count": 3, "type": "VEC3")",
                                 R"("count": 5000000, "type": "VEC3")"));
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("glTF index accessor over-declaring count is a typed ParseError, not an over-read") {
    auto result =
        io::importMesh(writeGltf("huge_index_count.gltf", R"("count": 3, "type": "SCALAR")",
                                 R"("count": 5000000, "type": "SCALAR")"));
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("glTF accessor whose byteOffset walks past the bufferView is a typed ParseError") {
    auto result = io::importMesh(
        writeGltf("bad_byte_offset.gltf", R"({"bufferView": 0, "componentType": 5126)",
                  R"({"bufferView": 0, "byteOffset": 32, "componentType": 5126)"));
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("glTF SCALAR POSITION accessor is rejected rather than read out of stride") {
    auto result = io::importMesh(writeGltf("scalar_position.gltf", R"("count": 3, "type": "VEC3")",
                                           R"("count": 3, "type": "SCALAR")"));
    REQUIRE(!result.ok());
    REQUIRE(result.error().code == io::ErrorCode::ParseError);
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
