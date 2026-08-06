// Sculpt handoff ingest (pipeline-bridge spec, "Sculpt handoff ingest").
#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cyber/core/io.hpp"
#include "cyber/handoff/handoff.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
using cyber::VertexId;
namespace handoff = cyber::handoff;
namespace io = cyber::io;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    const fs::path dir = fs::temp_directory_path() / "cyber_handoff_tests";
    fs::create_directories(dir);
    return dir;
}

struct HandoffVertex {
    Vec3 position;
    Vec3 normal;
    Vec3 color;  // 0..1
    float mix = 0.0f;
};

// The synthetic producer: writes the ASCII PLY profile of
// docs/sculpt-handoff-format.md. `major`/`minor` are parameters precisely so a
// test can emit a version this engine must reject.
std::string writeHandoffText(const std::vector<HandoffVertex>& vertices,
                             const std::vector<std::array<int, 3>>& triangles, int major, int minor,
                             const std::string& producer = "test-producer") {
    std::ostringstream out;
    out << "ply\nformat ascii 1.0\n";
    out << "comment cyber_sculpt_handoff " << major << " " << minor << "\n";
    if (!producer.empty()) {
        out << "comment cyber_handoff_producer " << producer << "\n";
    }
    out << "element vertex " << vertices.size() << "\n";
    out << "property float x\nproperty float y\nproperty float z\n";
    out << "property float nx\nproperty float ny\nproperty float nz\n";
    out << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    out << "property float material_mix\n";
    out << "element face " << triangles.size() << "\n";
    out << "property list uchar int vertex_indices\n";
    out << "end_header\n";
    for (const HandoffVertex& v : vertices) {
        out << v.position.x << " " << v.position.y << " " << v.position.z << " " << v.normal.x
            << " " << v.normal.y << " " << v.normal.z << " "
            << static_cast<int>(v.color.x * 255.0f + 0.5f) << " "
            << static_cast<int>(v.color.y * 255.0f + 0.5f) << " "
            << static_cast<int>(v.color.z * 255.0f + 0.5f) << " " << v.mix << "\n";
    }
    for (const auto& t : triangles) {
        out << "3 " << t[0] << " " << t[1] << " " << t[2] << "\n";
    }
    return out.str();
}

// A small coloured slab: two triangles per side of a box, enough to remesh,
// unwrap and bake.
struct Fixture {
    std::vector<HandoffVertex> vertices;
    std::vector<std::array<int, 3>> triangles;
};

Fixture makeBoxFixture() {
    Fixture f;
    const std::vector<Vec3> corners = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                       {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        HandoffVertex v;
        v.position = corners[i];
        v.normal = cyber::normalized(corners[i] - Vec3{0.5f, 0.5f, 0.5f});
        v.color = {1.0f, 0.0f, 0.0f};  // uniform red — a bakeable, checkable signal
        v.mix = static_cast<float>(i) / 8.0f;
        f.vertices.push_back(v);
    }
    const std::vector<std::array<int, 4>> quads = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                                   {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    for (const auto& q : quads) {
        f.triangles.push_back({q[0], q[1], q[2]});
        f.triangles.push_back({q[0], q[2], q[3]});
    }
    return f;
}

fs::path writeHandoffFile(const std::string& name, const std::string& text) {
    const fs::path path = tempDir() / name;
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

}  // namespace

TEST_CASE("a valid handoff file loads as a Target with its colors, normals and material mix") {
    const Fixture fixture = makeBoxFixture();
    const fs::path path = writeHandoffFile(
        "valid.ply", writeHandoffText(fixture.vertices, fixture.triangles, 1, 0, "claytest"));

    const auto result = handoff::readFile(path);
    REQUIRE(result.ok());
    const handoff::Handoff& h = result.value();
    CHECK(h.version.major == 1);
    CHECK(h.version.minor == 0);
    CHECK(h.producer == "claytest");
    CHECK(h.mesh.vertexCount() == 8);
    CHECK(h.mesh.faceCount() == 12);

    REQUIRE(h.hasVertexColors());
    REQUIRE(h.hasVertexNormals());
    REQUIRE(h.hasMaterialMix());
    // The per-VERTEX normal column must not be confused with the per-CORNER
    // "normal" attribute: same name, different attribute set.
    CHECK(h.mesh.cornerAttributes().find<Vec3>(io::kNormalAttribute) == nullptr);

    const auto* colors = h.mesh.vertexAttributes().find<Vec3>(io::kColorAttribute);
    REQUIRE(colors != nullptr);
    CHECK((*colors)[0].x == doctest::Approx(1.0f).epsilon(0.01));
    CHECK((*colors)[0].y == doctest::Approx(0.0f).epsilon(0.01));

    const auto* mix = h.mesh.vertexAttributes().find<float>(handoff::kMaterialMixAttribute);
    REQUIRE(mix != nullptr);
    CHECK((*mix)[3] == doctest::Approx(3.0f / 8.0f).epsilon(0.001));

    CHECK(h.boundsMin.x == doctest::Approx(0.0f));
    CHECK(h.boundsMax.z == doctest::Approx(1.0f));
}

TEST_CASE("an unsupported handoff version is rejected naming both versions") {
    const Fixture fixture = makeBoxFixture();
    const fs::path path =
        writeHandoffFile("future.ply", writeHandoffText(fixture.vertices, fixture.triangles, 2, 0));

    const auto result = handoff::readFile(path);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::IncompatibleVersion);
    const std::string& message = result.error().message;
    CHECK(message.find("2.0") != std::string::npos);
    CHECK(message.find("1.0") != std::string::npos);
}

TEST_CASE("a newer minor is rejected rather than read with the unknown parts dropped") {
    const Fixture fixture = makeBoxFixture();
    const fs::path path =
        writeHandoffFile("minor.ply", writeHandoffText(fixture.vertices, fixture.triangles, 1, 7));
    const auto result = handoff::readFile(path);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::IncompatibleVersion);
    CHECK(result.error().message.find("1.7") != std::string::npos);
}

TEST_CASE("a bad version beats every other defect to the error") {
    // A v2.0 header followed by geometry that could not possibly parse. If the
    // version gate ran anywhere but first, this would surface as a ParseError
    // and a partial Target might already exist.
    std::string text = "ply\nformat ascii 1.0\ncomment cyber_sculpt_handoff 2 0\n";
    text += "element vertex 3\nproperty float x\nproperty float y\nproperty float z\n";
    text += "element face 1\nproperty list uchar int vertex_indices\nend_header\n";
    text += "this is not a vertex\nnor is this\n???\n3 0 1 99999\n";
    const fs::path path = writeHandoffFile("garbage.ply", text);

    const auto result = handoff::readFile(path);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::IncompatibleVersion);
}

TEST_CASE("a PLY without the handoff comment is not a handoff") {
    const Fixture fixture = makeBoxFixture();
    std::string text = writeHandoffText(fixture.vertices, fixture.triangles, 1, 0);
    const std::size_t at = text.find("comment cyber_sculpt_handoff 1 0\n");
    REQUIRE(at != std::string::npos);
    text.erase(at, std::string("comment cyber_sculpt_handoff 1 0\n").size());
    const fs::path path = writeHandoffFile("plain.ply", text);

    const auto result = handoff::readFile(path);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::UnsupportedFormat);
}

TEST_CASE("a handoff missing a required vertex property is rejected") {
    std::string text = "ply\nformat ascii 1.0\ncomment cyber_sculpt_handoff 1 0\n";
    text += "element vertex 3\nproperty float x\nproperty float y\nproperty float z\n";
    text += "element face 1\nproperty list uchar int vertex_indices\nend_header\n";
    text += "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";
    const fs::path path = writeHandoffFile("nomix.ply", text);

    const auto result = handoff::readFile(path);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::ParseError);
    CHECK(result.error().message.find("material_mix") != std::string::npos);
}

TEST_CASE("a non-triangle handoff face is rejected rather than triangulated") {
    std::string text = "ply\nformat ascii 1.0\ncomment cyber_sculpt_handoff 1 0\n";
    text += "element vertex 4\nproperty float x\nproperty float y\nproperty float z\n";
    text += "property float nx\nproperty float ny\nproperty float nz\n";
    text += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    text += "property float material_mix\n";
    text += "element face 1\nproperty list uchar int vertex_indices\nend_header\n";
    text += "0 0 0 0 0 1 255 0 0 0\n1 0 0 0 0 1 255 0 0 0\n";
    text += "1 1 0 0 0 1 255 0 0 0\n0 1 0 0 0 1 255 0 0 0\n";
    text += "4 0 1 2 3\n";
    const fs::path path = writeHandoffFile("quadface.ply", text);

    const auto result = handoff::readFile(path);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::ParseError);
}

TEST_CASE("the buffer handoff and the file handoff agree") {
    const Fixture fixture = makeBoxFixture();
    const fs::path path = writeHandoffFile(
        "equiv.ply", writeHandoffText(fixture.vertices, fixture.triangles, 1, 0, "buffers"));
    const auto fileResult = handoff::readFile(path);
    REQUIRE(fileResult.ok());

    std::vector<float> positions, normals, colors, mix;
    for (const HandoffVertex& v : fixture.vertices) {
        positions.insert(positions.end(), {v.position.x, v.position.y, v.position.z});
        normals.insert(normals.end(), {v.normal.x, v.normal.y, v.normal.z});
        colors.insert(colors.end(), {v.color.x, v.color.y, v.color.z});
        mix.push_back(v.mix);
    }
    std::vector<std::uint32_t> indices;
    for (const auto& t : fixture.triangles) {
        indices.insert(indices.end(),
                       {static_cast<std::uint32_t>(t[0]), static_cast<std::uint32_t>(t[1]),
                        static_cast<std::uint32_t>(t[2])});
    }

    handoff::BufferView view;
    view.positions = positions.data();
    view.normals = normals.data();
    view.colors = colors.data();
    view.materialMix = mix.data();
    view.vertexCount = fixture.vertices.size();
    view.indices = indices.data();
    view.indexCount = indices.size();
    view.producer = "buffers";

    const auto bufferResult = handoff::readBuffers(view);
    REQUIRE(bufferResult.ok());

    const Mesh& a = fileResult.value().mesh;
    const Mesh& b = bufferResult.value().mesh;
    REQUIRE(a.vertexCount() == b.vertexCount());
    REQUIRE(a.faceCount() == b.faceCount());
    REQUIRE(a.edgeCount() == b.edgeCount());
    CHECK(bufferResult.value().producer == fileResult.value().producer);
    for (Index i = 0; i < a.vertexCapacity(); ++i) {
        CHECK(a.position(VertexId{i}).x == doctest::Approx(b.position(VertexId{i}).x));
        CHECK(a.position(VertexId{i}).y == doctest::Approx(b.position(VertexId{i}).y));
        CHECK(a.position(VertexId{i}).z == doctest::Approx(b.position(VertexId{i}).z));
    }
    const auto* colorA = a.vertexAttributes().find<Vec3>(io::kColorAttribute);
    const auto* colorB = b.vertexAttributes().find<Vec3>(io::kColorAttribute);
    REQUIRE(colorA != nullptr);
    REQUIRE(colorB != nullptr);
    CHECK((*colorA)[0].x == doctest::Approx((*colorB)[0].x).epsilon(0.005));
}

TEST_CASE("the buffer handoff is gated on the same version rule as a file") {
    const std::vector<float> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<std::uint32_t> indices = {0, 1, 2};
    handoff::BufferView view;
    view.positions = positions.data();
    view.vertexCount = 3;
    view.indices = indices.data();
    view.indexCount = 3;
    view.version = handoff::Version{9, 4};

    const auto result = handoff::readBuffers(view);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::IncompatibleVersion);
    CHECK(result.error().message.find("9.4") != std::string::npos);
}

TEST_CASE("a handoff reads from a stream as it does from a file") {
    const Fixture fixture = makeBoxFixture();
    const std::string text = writeHandoffText(fixture.vertices, fixture.triangles, 1, 0, "stdin");
    std::istringstream stream(text);
    const auto result = handoff::readStream(stream, "<stdin>");
    REQUIRE(result.ok());
    CHECK(result.value().mesh.vertexCount() == 8);
    CHECK(result.value().producer == "stdin");
}

TEST_CASE("a rejected stream handoff names the stream, not a file") {
    const Fixture fixture = makeBoxFixture();
    const std::string text = writeHandoffText(fixture.vertices, fixture.triangles, 3, 1);
    std::istringstream stream(text);
    const auto result = handoff::readStream(stream, "<stdin>");
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::IncompatibleVersion);
    CHECK(result.error().message.find("<stdin>") != std::string::npos);
    CHECK(result.error().message.find("3.1") != std::string::npos);
}
