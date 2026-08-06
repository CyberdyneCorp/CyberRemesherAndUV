// Sculpt handoff ingest (pipeline-bridge spec, "Sculpt handoff ingest").
#include <doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::string readAllBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// The mesh a glTF producer would export: positions + faces from the fixture,
// vertex colors, and normals on the CORNER set — the only place the engine's
// glTF writer reads NORMAL from.
Mesh makeGltfSourceMesh(const Fixture& fixture) {
    Mesh mesh;
    std::vector<VertexId> ids;
    ids.reserve(fixture.vertices.size());
    for (const HandoffVertex& v : fixture.vertices) {
        ids.push_back(mesh.addVertex(v.position));
    }
    auto& colors = mesh.vertexAttributes().create<Vec3>(io::kColorAttribute);
    for (std::size_t i = 0; i < fixture.vertices.size(); ++i) {
        colors[ids[i].value] = fixture.vertices[i].color;
    }
    auto& normals = mesh.cornerAttributes().create<Vec3>(io::kNormalAttribute);
    for (const auto& t : fixture.triangles) {
        const std::array<VertexId, 3> tri{ids[static_cast<std::size_t>(t[0])],
                                          ids[static_cast<std::size_t>(t[1])],
                                          ids[static_cast<std::size_t>(t[2])]};
        const FaceId f = mesh.addFace(tri);
        const std::vector<cyber::LoopId> loops = mesh.faceLoops(f);
        for (std::size_t c = 0; c < loops.size(); ++c) {
            normals[loops[c].value] = fixture.vertices[static_cast<std::size_t>(t[c])].normal;
        }
    }
    return mesh;
}

// The synthetic glTF producer's half that the engine's writer has no slot for:
// asset.extras.cyberSculptHandoff, exactly as docs/sculpt-handoff-format.md
// spells it.
std::string injectHandoffExtras(const std::string& json, int major, int minor,
                                const std::string& producer) {
    const std::size_t key = json.find("\"asset\"");
    const std::size_t brace = json.find('{', key);
    std::string extras = "\"extras\":{\"cyberSculptHandoff\":{\"major\":" + std::to_string(major) +
                         ",\"minor\":" + std::to_string(minor);
    if (!producer.empty()) {
        extras += ",\"producer\":\"" + producer + "\"";
    }
    extras += "}},";
    std::string out = json;
    out.insert(brace + 1, extras);
    return out;
}

std::uint32_t readU32(const std::string& bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

// Rebuilds a GLB around a patched JSON chunk: the chunk must stay 4-byte
// aligned and both the chunk length and the container length must follow it.
std::string patchGlbJson(const std::string& glb, int major, int minor,
                         const std::string& producer) {
    constexpr std::size_t kJsonChunkData = 20;  // 12 header + 4 length + 4 type
    const std::uint32_t jsonLength = readU32(glb, 12);
    std::string json =
        injectHandoffExtras(glb.substr(kJsonChunkData, jsonLength), major, minor, producer);
    while (json.size() % 4 != 0) {
        json.push_back(' ');
    }
    const std::string rest = glb.substr(kJsonChunkData + jsonLength);
    std::string out = glb.substr(0, kJsonChunkData) + json + rest;
    const auto total = static_cast<std::uint32_t>(out.size());
    const auto chunk = static_cast<std::uint32_t>(json.size());
    std::memcpy(out.data() + 8, &total, 4);
    std::memcpy(out.data() + 12, &chunk, 4);
    return out;
}

fs::path writeGltfHandoffFile(const std::string& name, const Mesh& mesh, int major, int minor,
                              const std::string& producer = "claytest") {
    const fs::path path = tempDir() / name;
    const io::Status status = io::exportMesh(mesh, path);
    REQUIRE(status.ok());
    std::string bytes = readAllBytes(path);
    bytes = path.extension() == ".glb" ? patchGlbJson(bytes, major, minor, producer)
                                       : injectHandoffExtras(bytes, major, minor, producer);
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
}

}  // namespace

TEST_CASE("a producer label that also occurs inside the comment key is read whole") {
    // Regression: the label was located with comment.find(<label>) over the
    // WHOLE line, so any label that is also a substring of the literal key
    // "cyber_handoff_producer" matched inside the key itself — producer "a"
    // came back as "andoff_producer a". Every pre-existing test used a label
    // ("claytest", "buffers", "stdin") that could not collide, so the bug was
    // invisible. Labels may also contain spaces, which the parse must keep.
    const Fixture fixture = makeBoxFixture();
    for (const std::string& label : {std::string("a"), std::string("producer"), std::string("c"),
                                     std::string("clay core 2")}) {
        const auto result = handoff::readFile(writeHandoffFile(
            "producer.ply", writeHandoffText(fixture.vertices, fixture.triangles, 1, 0, label)));
        REQUIRE(result.ok());
        CHECK(result.value().producer == label);
    }
}

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

TEST_CASE("a glTF handoff carries version, producer, geometry, colors and normals") {
    // The .gltf/.glb profile of docs/sculpt-handoff-format.md, end to end.
    // Regression: `producer` was declared in asset.extras and never read
    // (GltfDeclaration::producer was dead), and NORMAL landed only on the
    // CORNER set while Handoff::hasVertexNormals reads the VERTEX set — so a
    // file that declared both came back with neither.
    const Fixture fixture = makeBoxFixture();
    const Mesh source = makeGltfSourceMesh(fixture);
    for (const std::string& name : {std::string("handoff.gltf"), std::string("handoff.glb")}) {
        CAPTURE(name);
        const fs::path path = writeGltfHandoffFile(name, source, 1, 0, "clay core 2");
        const auto result = handoff::readFile(path);
        REQUIRE(result.ok());
        const handoff::Handoff& h = result.value();
        CHECK(h.version.major == 1);
        CHECK(h.version.minor == 0);
        CHECK(h.producer == "clay core 2");
        CHECK(h.mesh.vertexCount() == 8);
        CHECK(h.mesh.faceCount() == 12);
        CHECK(h.droppedFaces == 0);

        REQUIRE(h.hasVertexColors());
        REQUIRE(h.hasVertexNormals());
        const auto* colors = h.mesh.vertexAttributes().find<Vec3>(io::kColorAttribute);
        REQUIRE(colors != nullptr);
        CHECK((*colors)[0].x == doctest::Approx(1.0f).epsilon(0.01));
        CHECK((*colors)[0].y == doctest::Approx(0.0f).epsilon(0.01));

        // The glTF writer splits vertices per distinct corner tuple, so vertex
        // order is not the fixture's: match the normal by position instead.
        const auto* normals = h.mesh.vertexAttributes().find<Vec3>(io::kNormalAttribute);
        REQUIRE(normals != nullptr);
        for (Index i = 0; i < h.mesh.vertexCapacity(); ++i) {
            if (!h.mesh.isAlive(VertexId{i})) {
                continue;
            }
            const Vec3 expected =
                cyber::normalized(h.mesh.position(VertexId{i}) - Vec3{0.5f, 0.5f, 0.5f});
            CHECK((*normals)[i].x == doctest::Approx(expected.x).epsilon(0.001));
            CHECK((*normals)[i].y == doctest::Approx(expected.y).epsilon(0.001));
            CHECK((*normals)[i].z == doctest::Approx(expected.z).epsilon(0.001));
        }

        // material_mix has no glTF core slot: accepted, but never silently.
        CHECK(!h.hasMaterialMix());
        bool warned = false;
        for (const std::string& warning : h.warnings) {
            warned = warned || warning.find(handoff::kMaterialMixAttribute) != std::string::npos;
        }
        CHECK(warned);
    }
}

TEST_CASE("a glTF handoff is gated on the same version rule as a PLY one") {
    const Fixture fixture = makeBoxFixture();
    const Mesh source = makeGltfSourceMesh(fixture);
    for (const std::string& name : {std::string("future.gltf"), std::string("future.glb")}) {
        CAPTURE(name);
        const auto result = handoff::readFile(writeGltfHandoffFile(name, source, 2, 0));
        REQUIRE(!result.ok());
        CHECK(result.error().code == io::ErrorCode::IncompatibleVersion);
        CHECK(result.error().message.find("2.0") != std::string::npos);
        CHECK(result.error().message.find("1.0") != std::string::npos);
    }
    for (const std::string& name : {std::string("minor.gltf"), std::string("minor.glb")}) {
        CAPTURE(name);
        const auto result = handoff::readFile(writeGltfHandoffFile(name, source, 1, 7));
        REQUIRE(!result.ok());
        CHECK(result.error().code == io::ErrorCode::IncompatibleVersion);
        CHECK(result.error().message.find("1.7") != std::string::npos);
    }
}

TEST_CASE("a glTF without the handoff declaration is not a handoff") {
    const Fixture fixture = makeBoxFixture();
    const fs::path path = tempDir() / "plain.gltf";
    REQUIRE(io::exportMesh(makeGltfSourceMesh(fixture), path).ok());
    const auto result = handoff::readFile(path);
    REQUIRE(!result.ok());
    CHECK(result.error().code == io::ErrorCode::UnsupportedFormat);
}

TEST_CASE("a handoff triangle with a repeated vertex index is reported, not dropped in silence") {
    // Mesh::addFace refuses a face with a repeated vertex — a routine defect in
    // sculpt exports. Regression: both the PLY and the buffer route discarded
    // that FaceId, so the ingest reported success with fewer faces than the
    // file declared and nothing said so.
    const Fixture fixture = makeBoxFixture();
    Fixture broken = fixture;
    broken.triangles.push_back({2, 2, 5});  // degenerate: two corners on one vertex
    const fs::path path = writeHandoffFile(
        "degenerate.ply", writeHandoffText(broken.vertices, broken.triangles, 1, 0, "claytest"));

    const auto fileResult = handoff::readFile(path);
    REQUIRE(fileResult.ok());
    CHECK(fileResult.value().mesh.faceCount() == 12);
    CHECK(fileResult.value().droppedFaces == 1);
    bool warned = false;
    for (const std::string& warning : fileResult.value().warnings) {
        warned = warned || warning.find("1") != std::string::npos;
    }
    CHECK(warned);

    std::vector<float> positions, normals, colors, mix;
    for (const HandoffVertex& v : broken.vertices) {
        positions.insert(positions.end(), {v.position.x, v.position.y, v.position.z});
        normals.insert(normals.end(), {v.normal.x, v.normal.y, v.normal.z});
        colors.insert(colors.end(), {v.color.x, v.color.y, v.color.z});
        mix.push_back(v.mix);
    }
    std::vector<std::uint32_t> indices;
    for (const auto& t : broken.triangles) {
        indices.insert(indices.end(),
                       {static_cast<std::uint32_t>(t[0]), static_cast<std::uint32_t>(t[1]),
                        static_cast<std::uint32_t>(t[2])});
    }
    handoff::BufferView view;
    view.positions = positions.data();
    view.normals = normals.data();
    view.colors = colors.data();
    view.materialMix = mix.data();
    view.vertexCount = broken.vertices.size();
    view.indices = indices.data();
    view.indexCount = indices.size();

    const auto bufferResult = handoff::readBuffers(view);
    REQUIRE(bufferResult.ok());
    CHECK(bufferResult.value().mesh.faceCount() == 12);
    CHECK(bufferResult.value().droppedFaces == 1);
    CHECK(!bufferResult.value().warnings.empty());
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
