#include <doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "cyber/app/document.hpp"
#include "cyber/app/input.hpp"
#include "cyber/app/shell_desktop.hpp"
#include "cyber/app/undo.hpp"
#include "cyber/core/mesh.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
namespace app = cyber::app;

namespace {

Mesh makeQuadMesh() {
    const std::vector<Vec3> positions = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    const std::vector<std::vector<Index>> faces = {{0, 1, 2, 3}};
    return Mesh::fromIndexed(positions, faces);
}

// 3x3 vertex grid of four quads; vertex id of (x, y) is y * 3 + x.
Mesh makeGridMesh() {
    std::vector<Vec3> positions;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            positions.push_back(Vec3{static_cast<float>(x), static_cast<float>(y), 0.0f});
        }
    }
    const std::vector<std::vector<Index>> faces = {
        {0, 1, 4, 3}, {1, 2, 5, 4}, {3, 4, 7, 6}, {4, 5, 8, 7}};
    return Mesh::fromIndexed(positions, faces);
}

Mesh makeTriMesh() {
    const std::vector<Vec3> positions = {
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
    const std::vector<std::vector<Index>> faces = {{0, 1, 2}};
    return Mesh::fromIndexed(positions, faces);
}

// ---- attribute painting, keyed to positions so ids may be renumbered -------

Vec2 uvFor(Vec3 p) { return Vec2{p.x, p.y}; }
Vec3 colorFor(Vec3 p) { return Vec3{p.x, p.y, 0.5f}; }

// One column of every element kind that survives compaction by position:
// vertex colours and corner UVs derived from the vertex position, plus a face
// tag numbered in face order.
void paintAttributes(Mesh& mesh) {
    auto& colors = mesh.vertexAttributes().create<Vec3>("color");
    for (std::size_t i = 0; i < mesh.vertexCapacity(); ++i) {
        const cyber::VertexId v{static_cast<Index>(i)};
        if (mesh.isAlive(v)) {
            colors[i] = colorFor(mesh.position(v));
        }
    }
    auto& uvs = mesh.cornerAttributes().create<Vec2>("uv");
    auto& tags = mesh.faceAttributes().create<std::int32_t>("mat");
    for (std::size_t i = 0; i < mesh.faceCapacity(); ++i) {
        const cyber::FaceId f{static_cast<Index>(i)};
        if (!mesh.isAlive(f)) {
            continue;
        }
        tags[i] = static_cast<std::int32_t>(i) + 1;
        for (const cyber::LoopId l : mesh.faceLoops(f)) {
            uvs[l.value] = uvFor(mesh.position(mesh.loopVertex(l)));
        }
    }
}

cyber::VertexId vertexAt(const Mesh& mesh, Vec3 p) {
    for (std::size_t i = 0; i < mesh.vertexCapacity(); ++i) {
        const cyber::VertexId v{static_cast<Index>(i)};
        if (mesh.isAlive(v) && mesh.position(v) == p) {
            return v;
        }
    }
    return {};
}

cyber::EdgeId edgeAt(const Mesh& mesh, Vec3 a, Vec3 b) {
    return mesh.edgeBetween(vertexAt(mesh, a), vertexAt(mesh, b));
}

std::size_t featureEdgeCount(const Mesh& mesh) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < mesh.edgeCapacity(); ++i) {
        const cyber::EdgeId e{static_cast<Index>(i)};
        if (mesh.isAlive(e) && mesh.isFeatureEdge(e)) {
            ++count;
        }
    }
    return count;
}

// Checks the persisted columns against the positions they were derived from,
// which operator== cannot do: it compares meshes structurally by design.
void checkPaintedAttributes(const Mesh& mesh) {
    const auto* colors = mesh.vertexAttributes().find<Vec3>("color");
    REQUIRE(colors != nullptr);
    for (std::size_t i = 0; i < mesh.vertexCapacity(); ++i) {
        const cyber::VertexId v{static_cast<Index>(i)};
        if (mesh.isAlive(v)) {
            CHECK((*colors)[i] == colorFor(mesh.position(v)));
        }
    }
    const auto* uvs = mesh.cornerAttributes().find<Vec2>("uv");
    REQUIRE(uvs != nullptr);
    for (std::size_t i = 0; i < mesh.faceCapacity(); ++i) {
        const cyber::FaceId f{static_cast<Index>(i)};
        if (!mesh.isAlive(f)) {
            continue;
        }
        for (const cyber::LoopId l : mesh.faceLoops(f)) {
            CHECK((*uvs)[l.value] == uvFor(mesh.position(mesh.loopVertex(l))));
        }
    }
}

// ---- little-endian probes into a saved document container ------------------

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
    }
}

std::uint64_t readU64(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(bytes[at + i]) << (i * 8);
    }
    return v;
}

// Section ids in write order (header is magic + version + section count).
std::vector<std::uint32_t> sectionIds(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint32_t> ids;
    std::size_t at = 12;
    for (std::uint32_t i = 0; i < readU32(bytes, 8); ++i) {
        ids.push_back(readU32(bytes, at));
        at += 12 + static_cast<std::size_t>(readU64(bytes, at + 4));
    }
    return ids;
}

// Byte offset of the section header with `id`, or bytes.size() if absent.
std::size_t sectionOffset(const std::vector<std::uint8_t>& bytes, std::uint32_t id) {
    std::size_t at = 12;
    for (std::uint32_t i = 0; i < readU32(bytes, 8); ++i) {
        if (readU32(bytes, at) == id) {
            return at;
        }
        at += 12 + static_cast<std::size_t>(readU64(bytes, at + 4));
    }
    return bytes.size();
}

// A command that adds a delta to a shared counter with a configurable memory
// cost, used to exercise the budgeted journal.
struct AddValueCommand : app::Command {
    AddValueCommand(int* counter, int delta, std::size_t bytes)
        : m_counter(counter), m_delta(delta), m_bytes(bytes) {}

    void apply() override { *m_counter += m_delta; }
    void revert() override { *m_counter -= m_delta; }
    [[nodiscard]] std::size_t estimatedBytes() const override { return m_bytes; }
    [[nodiscard]] std::string label() const override { return "add"; }

    int* m_counter;
    int m_delta;
    std::size_t m_bytes;
};

}  // namespace

TEST_CASE("document save/load round-trip is equal") {
    app::Document doc;
    doc.target = makeTriMesh();
    doc.editMesh = makeQuadMesh();
    doc.params.targetQuadCount = 12345;
    doc.params.edgeScale = 2.5f;
    doc.params.pureQuads = true;
    doc.params.smallPatchPolicy = cyber::remesh::SmallPatchPolicy::MinFaces;
    doc.params.smallPatchMinFaces = 7;
    doc.bake.map = app::BakeMapKind::AmbientOcclusion;
    doc.bake.width = 2048;
    doc.bake.height = 1024;
    doc.bake.cageDistance = 0.25f;
    doc.bake.baked = true;
    doc.bake.bakeRevision = 42;

    const std::vector<std::uint8_t> buffer = doc.save();
    const auto loaded = app::Document::load(buffer);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == doc);
}

// ---- named soft-selection slots (add-soft-selection, task 2) ---------------

TEST_CASE("named soft selections round-trip through the document") {
    app::Document doc;
    doc.editMesh = makeQuadMesh();
    doc.softSelections["taper"] = {0.0f, 0.25f, 0.5f, 1.0f};
    doc.softSelections["arm"] = {1.0f, 1.0f};

    const std::vector<std::uint8_t> buffer = doc.save();
    const auto loaded = app::Document::load(buffer);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == doc);
    REQUIRE(loaded->softSelections.size() == 2);
    CHECK(loaded->softSelections.at("taper") == doc.softSelections.at("taper"));
    CHECK(loaded->softSelections.at("arm") == doc.softSelections.at("arm"));
}

// Regression: the mesh travels through toIndexed, which drops dead vertices and
// renumbers the survivors, while slot weights are indexed by vertex id. Without
// a matching rebase, a document saved after any deletion reloaded with every
// weight shifted by the hole — silently selecting geometry the user never
// painted. The weights must still sit on the same POSITIONS after the trip.
TEST_CASE("soft-selection slots follow the compacted ids across a deleted vertex") {
    app::Document doc;
    doc.editMesh = makeGridMesh();
    doc.editMesh.removeFace(cyber::FaceId{0});
    REQUIRE(doc.editMesh.removeIsolatedVertex(cyber::VertexId{0}));
    REQUIRE(doc.editMesh.vertexCount() == 8);
    REQUIRE(doc.editMesh.vertexCapacity() == 9);  // id 0 is now a hole

    // Paint the far corner block: ids 4, 5, 7, 8 = (1,1), (2,1), (1,2), (2,2).
    const std::vector<Vec3> painted = {
        {1.0f, 1.0f, 0.0f}, {2.0f, 1.0f, 0.0f}, {1.0f, 2.0f, 0.0f}, {2.0f, 2.0f, 0.0f}};
    std::vector<float> weights(doc.editMesh.vertexCapacity(), 0.0f);
    for (const Index id : {4u, 5u, 7u, 8u}) {
        weights[id] = 1.0f;
    }
    doc.softSelections["region"] = weights;

    const auto loaded = app::Document::load(doc.save());
    REQUIRE(loaded.has_value());

    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    loaded->editMesh.toIndexed(positions, faces);
    const std::vector<float>& restored = loaded->softSelections.at("region");
    CHECK(restored.size() == positions.size());  // one weight per live vertex

    for (std::size_t i = 0; i < positions.size(); ++i) {
        const bool wanted =
            std::find(painted.begin(), painted.end(), positions[i]) != painted.end();
        const float got = i < restored.size() ? restored[i] : 0.0f;
        CHECK(got == doctest::Approx(wanted ? 1.0f : 0.0f));
    }
    CHECK(*loaded == doc);
}

// ---- mesh attributes and feature edges -------------------------------------

// Regression: save() serialised positions and face indices only, so every
// attribute column (corner UVs, vertex colours) and every feature-edge tag was
// dropped by a save/load round trip while the in-memory document kept them —
// the loss showed up only after reloading, with no error on any call. The check
// must look at what is persisted, since operator== compares meshes structurally.
TEST_CASE("mesh attributes and feature edges survive a document round trip") {
    app::Document doc;
    doc.target = makeTriMesh();
    doc.editMesh = makeGridMesh();
    paintAttributes(doc.editMesh);
    auto& creases = doc.editMesh.edgeAttributes().create<std::int32_t>("crease");
    const cyber::EdgeId tagged =
        edgeAt(doc.editMesh, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});
    REQUIRE(tagged.valid());
    doc.editMesh.setFeatureEdge(tagged, true);
    creases[tagged.value] = 7;

    const std::vector<std::uint8_t> buffer = doc.save();
    // Append-only: the attribute section takes the next unused id and is
    // emitted only for the mesh that carries data (the target has none).
    CHECK(sectionIds(buffer) == std::vector<std::uint32_t>{1u, 2u, 3u, 4u, 7u});
    CHECK(readU32(buffer, 4) == app::Document::kFormatVersion);

    const auto loaded = app::Document::load(buffer);
    REQUIRE(loaded.has_value());
    const Mesh& mesh = loaded->editMesh;
    checkPaintedAttributes(mesh);

    const auto* tags = mesh.faceAttributes().find<std::int32_t>("mat");
    REQUIRE(tags != nullptr);
    for (std::size_t i = 0; i < mesh.faceCount(); ++i) {
        CHECK((*tags)[i] == static_cast<std::int32_t>(i) + 1);
    }

    const cyber::EdgeId reloaded = edgeAt(mesh, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f});
    REQUIRE(reloaded.valid());
    CHECK(mesh.isFeatureEdge(reloaded));
    CHECK(featureEdgeCount(mesh) == 1);  // and no other edge came back tagged
    const auto* loadedCreases = mesh.edgeAttributes().find<std::int32_t>("crease");
    REQUIRE(loadedCreases != nullptr);
    CHECK((*loadedCreases)[reloaded.value] == 7);
}

// The rows are keyed by element id, but the mesh travels through toIndexed,
// which drops dead elements and renumbers the survivors — so the columns are
// persisted in that same compacted order and must land back on the elements
// holding the positions they were painted on.
TEST_CASE("attribute rows follow the compacted ids across a deleted face") {
    app::Document doc;
    doc.editMesh = makeGridMesh();
    doc.editMesh.removeFace(cyber::FaceId{0});
    REQUIRE(doc.editMesh.removeIsolatedVertex(cyber::VertexId{0}));
    paintAttributes(doc.editMesh);
    const cyber::EdgeId tagged =
        edgeAt(doc.editMesh, Vec3{1.0f, 2.0f, 0.0f}, Vec3{2.0f, 2.0f, 0.0f});
    REQUIRE(tagged.valid());
    doc.editMesh.setFeatureEdge(tagged, true);

    const auto loaded = app::Document::load(doc.save());
    REQUIRE(loaded.has_value());
    const Mesh& mesh = loaded->editMesh;
    REQUIRE(mesh.vertexCount() == 8);
    REQUIRE(mesh.faceCount() == 3);
    checkPaintedAttributes(mesh);

    // Face 0 was removed, so the survivors keep their tags 2, 3, 4 in order.
    const auto* tags = mesh.faceAttributes().find<std::int32_t>("mat");
    REQUIRE(tags != nullptr);
    CHECK(std::vector<std::int32_t>(tags->begin(), tags->begin() + 3) ==
          std::vector<std::int32_t>{2, 3, 4});

    const cyber::EdgeId reloaded = edgeAt(mesh, Vec3{1.0f, 2.0f, 0.0f}, Vec3{2.0f, 2.0f, 0.0f});
    REQUIRE(reloaded.valid());
    CHECK(mesh.isFeatureEdge(reloaded));
    CHECK(featureEdgeCount(mesh) == 1);
}

// Corner rows are linearised face by face, so a mesh of mixed arity is the
// case that catches an off-by-one in that walk.
TEST_CASE("corner attributes survive faces of mixed arity") {
    const std::vector<Vec3> positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f},
                                         {3.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
                                         {2.0f, 1.0f, 0.0f}, {3.0f, 1.0f, 0.0f}};
    const std::vector<std::vector<Index>> faces = {{0, 1, 5}, {1, 2, 6, 5}, {2, 3, 7, 6, 5}};

    app::Document doc;
    doc.editMesh = Mesh::fromIndexed(positions, faces);
    paintAttributes(doc.editMesh);

    const auto loaded = app::Document::load(doc.save());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->editMesh.faceCount() == 3);
    checkPaintedAttributes(loaded->editMesh);
}

TEST_CASE("document load rejects absurd attribute counts without crashing") {
    app::Document doc;
    doc.editMesh = makeGridMesh();
    paintAttributes(doc.editMesh);

    const std::vector<std::uint8_t> buffer = doc.save();
    const std::size_t offset = sectionOffset(buffer, 7u);
    REQUIRE(offset != buffer.size());
    const std::size_t payload = offset + 12u;  // section id u32 + length u64

    std::vector<std::uint8_t> badColumns = buffer;
    writeU32(badColumns, payload, 0xFFFFFFFFu);  // vertex column count
    CHECK_FALSE(app::Document::load(badColumns).has_value());

    // payload = [colCount u32][name len u32]["color"][type tag u8][rowCount u32]
    const std::size_t rowCount = payload + 4u + 4u + 5u + 1u;
    std::vector<std::uint8_t> badRows = buffer;
    writeU32(badRows, rowCount, 0xFFFFFFFFu);
    CHECK_FALSE(app::Document::load(badRows).has_value());

    // A column carries one row per element, and a short one is refused rather
    // than accepted: otherwise a small file could declare a great many columns
    // costing 9 bytes each and make the load allocate a full-length column for
    // every one of them.
    std::vector<std::uint8_t> shortRows = buffer;
    writeU32(shortRows, rowCount, 0u);
    CHECK_FALSE(app::Document::load(shortRows).has_value());
}

// The slot section is append-only: it is emitted only when slots exist, so a
// document without them keeps the exact byte layout earlier builds wrote —
// four sections, ids 1..4, format version unchanged.
TEST_CASE("a document without slots keeps the pre-change byte layout") {
    app::Document doc;
    doc.target = makeTriMesh();
    doc.editMesh = makeQuadMesh();
    REQUIRE(doc.softSelections.empty());

    const std::vector<std::uint8_t> buffer = doc.save();
    const std::vector<std::uint32_t> ids = sectionIds(buffer);
    CHECK(readU32(buffer, 4) == app::Document::kFormatVersion);
    CHECK(readU32(buffer, 8) == 4u);
    CHECK(ids == std::vector<std::uint32_t>{1u, 2u, 3u, 4u});

    doc.softSelections["taper"] = {0.5f};
    const std::vector<std::uint8_t> withSlots = doc.save();
    CHECK(readU32(withSlots, 4) == app::Document::kFormatVersion);
    CHECK(readU32(withSlots, 8) == 5u);
    CHECK(sectionIds(withSlots) == std::vector<std::uint32_t>{1u, 2u, 3u, 4u, 5u});
}

// An OLDER binary sees the slot section as an unknown id and must skip it by
// its length prefix rather than fail the load. Rewriting the id to one no
// build knows reproduces exactly that reader.
TEST_CASE("an unknown trailing section is skipped and the rest still loads") {
    app::Document doc;
    doc.target = makeTriMesh();
    doc.editMesh = makeQuadMesh();
    doc.params.targetQuadCount = 999;
    doc.bake.bakeRevision = 7;
    doc.softSelections["taper"] = {0.0f, 1.0f};

    std::vector<std::uint8_t> buffer = doc.save();
    const std::size_t offset = sectionOffset(buffer, 5u);
    REQUIRE(offset != buffer.size());
    writeU32(buffer, offset, 0xFEEDu);  // an id no reader knows

    const auto loaded = app::Document::load(buffer);
    REQUIRE(loaded.has_value());
    CHECK(loaded->softSelections.empty());
    CHECK(loaded->params.targetQuadCount == 999);
    CHECK(loaded->bake.bakeRevision == 7);
    CHECK(loaded->editMesh.faceCount() == doc.editMesh.faceCount());
}

TEST_CASE("document load rejects an absurd soft-selection weight count") {
    app::Document doc;
    doc.editMesh = makeQuadMesh();
    doc.softSelections["taper"] = {0.0f, 1.0f};

    std::vector<std::uint8_t> buffer = doc.save();
    const std::size_t offset = sectionOffset(buffer, 5u);
    REQUIRE(offset != buffer.size());
    // payload = [slotCount u32][name len u32]["taper"][weightCount u32]...
    const std::size_t weightCount = offset + 12u + 4u + 4u + 5u;
    writeU32(buffer, weightCount, 0xFFFFFFFFu);
    CHECK_FALSE(app::Document::load(buffer).has_value());
}

TEST_CASE("document load rejects a bad magic and truncation") {
    app::Document doc;
    doc.editMesh = makeQuadMesh();
    std::vector<std::uint8_t> buffer = doc.save();

    // Corrupt magic.
    std::vector<std::uint8_t> bad = buffer;
    bad[0] ^= 0xFFu;
    CHECK_FALSE(app::Document::load(bad).has_value());

    // Truncate.
    buffer.resize(buffer.size() / 2);
    CHECK_FALSE(app::Document::load(buffer).has_value());
}

// Regression: a corrupt file whose mesh length fields are absurd (e.g. a
// vertexCount of 0xFFFFFFFF) must be rejected, not reserve() gigabytes and
// abort with bad_alloc/length_error. The reader validates each count against
// the bytes actually remaining before reserving.
TEST_CASE("document load rejects an absurd vertex/face count without crashing") {
    app::Document doc;
    doc.editMesh = makeQuadMesh();
    const std::vector<std::uint8_t> good = doc.save();

    // Locate the EditMesh section's vertexCount and overwrite it with a huge
    // value. The section layout is [id u32][len u64][vertexCount u32]...; scan
    // for the EditMesh section id (2) that a saved quad mesh produces.
    const auto patchU32 = [](std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
        b[at + 0] = static_cast<std::uint8_t>(v & 0xFFu);
        b[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        b[at + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
        b[at + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    };

    // Brute-force every 4-byte-aligned offset: set it to a huge count and
    // confirm load never crashes (it returns nullopt or a valid document, but
    // never throws or aborts). This covers the vertex, face, and arity fields.
    for (std::size_t off = 12; off + 4 <= good.size(); ++off) {
        std::vector<std::uint8_t> corrupt = good;
        patchU32(corrupt, off, 0xFFFFFFFFu);
        auto loaded = app::Document::load(corrupt);
        (void)loaded;  // must not throw/abort regardless of the outcome
    }

    // And a direct, targeted case: a minimal valid header with a single section
    // declaring a vertexCount far larger than its payload.
    std::vector<std::uint8_t> forged = good;
    // Overwrite the first mesh section's vertexCount (first u32 after the
    // section id+length, which begins at byte 12: 3 header u32s = 12 bytes,
    // then Target section [id u32][len u64] = 12 more -> vertexCount at 24).
    if (forged.size() > 28) {
        patchU32(forged, 24, 0x7FFFFFFFu);
        CHECK_FALSE(app::Document::load(forged).has_value());
    }
}

// Regression: the section length is an attacker-controlled u64. A value near
// 2^64 used to wrap the reader's `m_pos + n` bounds check, so `sub()` handed
// readMesh a span far longer than the file and every subsequent read walked off
// the end of the heap block.
TEST_CASE("document load rejects a section length that wraps the bounds check") {
    std::vector<std::uint8_t> forged;
    const auto appendU32 = [&forged](std::uint32_t v) {
        for (std::size_t i = 0; i < 4; ++i) {
            forged.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
        }
    };
    const auto appendU64 = [&forged](std::uint64_t v) {
        for (std::size_t i = 0; i < 8; ++i) {
            forged.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
        }
    };

    appendU32(app::Document::kMagic);
    appendU32(app::Document::kFormatVersion);
    appendU32(1u);  // one section
    appendU32(2u);  // SectionId::EditMesh
    // 2^64 - 24: the cursor sits at byte 24, so cursor + length wraps to 0.
    appendU64(~std::uint64_t{0} - 23u);
    appendU32(1000u);  // vertexCount the over-long span would have accepted
    REQUIRE(forged.size() == 28u);

    CHECK_FALSE(app::Document::load(forged).has_value());

    // A length that merely overruns the file, without wrapping, is rejected too.
    writeU32(forged, 16, 0xFFFFFFFFu);
    writeU32(forged, 20, 0u);
    CHECK_FALSE(app::Document::load(forged).has_value());
}

TEST_CASE("autosave fires only when dirty") {
    app::Document doc;
    doc.editMesh = makeQuadMesh();

    int saves = 0;
    app::Document out;
    const auto sink = [&](std::span<const std::uint8_t> bytes) {
        ++saves;
        auto loaded = app::Document::load(bytes);
        REQUIRE(loaded.has_value());
        out = std::move(*loaded);
    };

    CHECK_FALSE(doc.autosaveIfDirty(sink));  // clean: no save
    CHECK(saves == 0);

    doc.markDirty();
    CHECK(doc.autosaveIfDirty(sink));  // dirty: saves and clears
    CHECK(saves == 1);
    CHECK_FALSE(doc.dirty());
    CHECK(out == doc);

    CHECK_FALSE(doc.autosaveIfDirty(sink));  // no longer dirty
    CHECK(saves == 1);
}

TEST_CASE("undo/redo respects the memory budget, evicting oldest") {
    int value = 0;
    app::UndoStack stack(100);  // budget = 100 bytes

    for (int i = 0; i < 5; ++i) {  // five commands, 30 bytes each -> 150 total
        stack.push(std::make_unique<AddValueCommand>(&value, 1, std::size_t{30}));
    }

    CHECK(value == 5);
    CHECK(stack.undoDepth() == 3);     // only three fit under the budget
    CHECK(stack.evictedCount() == 2);  // two oldest were evicted
    CHECK(stack.usedBytes() == 90);

    // The three surviving commands undo; the evicted two cannot.
    CHECK(stack.undo());
    CHECK(stack.undo());
    CHECK(stack.undo());
    CHECK_FALSE(stack.undo());
    CHECK(value == 2);  // stuck at the evicted floor

    // Redo restores them.
    CHECK(stack.redo());
    CHECK(stack.redo());
    CHECK(stack.redo());
    CHECK_FALSE(stack.redo());
    CHECK(value == 5);
}

TEST_CASE("pushing a new command clears the redo branch") {
    int value = 0;
    app::UndoStack stack(1000);
    stack.push(std::make_unique<AddValueCommand>(&value, 10, std::size_t{8}));
    stack.push(std::make_unique<AddValueCommand>(&value, 100, std::size_t{8}));
    CHECK(value == 110);

    CHECK(stack.undo());
    CHECK(value == 10);
    CHECK(stack.canRedo());

    stack.push(std::make_unique<AddValueCommand>(&value, 1, std::size_t{8}));
    CHECK(value == 11);
    CHECK_FALSE(stack.canRedo());  // redo branch discarded
}

TEST_CASE("journal metadata round-trips through the byte writer") {
    int value = 0;
    app::UndoStack stack(1000);
    stack.push(std::make_unique<AddValueCommand>(&value, 1, std::size_t{16}));
    stack.push(std::make_unique<AddValueCommand>(&value, 1, std::size_t{16}));
    stack.undo();

    app::ByteWriter w;
    stack.serializeMetadata(w);
    app::ByteReader r(w.data());
    const auto meta = app::UndoStack::loadMetadata(r);
    REQUIRE(meta.has_value());
    CHECK(meta->undo.size() == 1);
    CHECK(meta->redo.size() == 1);
    CHECK(meta->undo == stack.metadata().undo);
    CHECK(meta->redo == stack.metadata().redo);
}

// Regression: readEntries reserved an entry count taken straight off the wire,
// so a 4-byte journal asked std::vector for ~171 GB and the uncaught
// std::bad_alloc aborted the process. Counts are now validated against the
// bytes left, and a truncated journal yields nullopt instead of a partial one.
TEST_CASE("journal load rejects a corrupt entry count without allocating") {
    const std::vector<std::uint8_t> countOnly = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
    app::ByteReader huge(countOnly);
    CHECK_FALSE(app::UndoStack::loadMetadata(huge).has_value());

    int value = 0;
    app::UndoStack stack(1000);
    stack.push(std::make_unique<AddValueCommand>(&value, 1, std::size_t{16}));
    stack.push(std::make_unique<AddValueCommand>(&value, 1, std::size_t{16}));
    app::ByteWriter w;
    stack.serializeMetadata(w);

    // More entries claimed than the buffer could ever hold.
    std::vector<std::uint8_t> inflated = w.data();
    writeU32(inflated, 0, 1000u);
    app::ByteReader inflatedReader(inflated);
    CHECK_FALSE(app::UndoStack::loadMetadata(inflatedReader).has_value());

    // Truncated mid-entry: rejected rather than returning a partial snapshot.
    std::vector<std::uint8_t> truncated = w.data();
    truncated.resize(8);
    app::ByteReader truncatedReader(truncated);
    CHECK_FALSE(app::UndoStack::loadMetadata(truncatedReader).has_value());
}

TEST_CASE("double-tap recognizer fires on close, quick taps") {
    app::DoubleTapRecognizer rec;  // default: 0.3 s, 24 pts

    CHECK_FALSE(rec.tap(Vec2{0.0f, 0.0f}, 0.0));
    CHECK(rec.tap(Vec2{2.0f, 2.0f}, 0.1));  // within time and distance -> fires

    // Too slow: second tap after the interval does not fire.
    CHECK_FALSE(rec.tap(Vec2{0.0f, 0.0f}, 1.0));
    CHECK_FALSE(rec.tap(Vec2{0.0f, 0.0f}, 1.5));

    // Too far: within time but beyond the distance threshold.
    CHECK_FALSE(rec.tap(Vec2{0.0f, 0.0f}, 2.0));
    CHECK_FALSE(rec.tap(Vec2{100.0f, 100.0f}, 2.05));
}

TEST_CASE("press-hold recognizer fires after the hold duration") {
    app::PressHoldRecognizer rec;  // default: 0.5 s, 12 pts

    rec.press(Vec2{0.0f, 0.0f}, 0.0);
    CHECK_FALSE(rec.update(0.2));       // not yet
    CHECK(rec.move(Vec2{3.0f, 3.0f}));  // small move within slop is fine
    CHECK(rec.update(0.5));             // fires exactly once at the threshold
    CHECK(rec.fired());
    CHECK_FALSE(rec.update(0.6));  // does not fire again
}

TEST_CASE("press-hold is cancelled by movement beyond the slop") {
    app::PressHoldRecognizer rec;

    rec.press(Vec2{0.0f, 0.0f}, 0.0);
    CHECK_FALSE(rec.move(Vec2{50.0f, 0.0f}));  // dragged too far -> cancelled
    CHECK_FALSE(rec.update(1.0));              // never fires
    CHECK_FALSE(rec.fired());
}

TEST_CASE("stroke capture records timed pressure samples") {
    app::StrokeCapture capture;
    CHECK_FALSE(capture.active());

    capture.begin(Vec2{0.0f, 0.0f}, 0.5f, 0.0);
    CHECK(capture.active());
    capture.extend(Vec2{3.0f, 4.0f}, 0.6f, 0.1);  // 5 units from origin
    const app::Stroke stroke = capture.end(Vec2{3.0f, 4.0f}, 0.7f, 0.2);

    CHECK_FALSE(capture.active());
    CHECK(stroke.size() == 3);
    CHECK(stroke.duration() == doctest::Approx(0.2));
    CHECK(stroke.arcLength() == doctest::Approx(5.0f));
    CHECK(stroke.samples.front().pressure == doctest::Approx(0.5f));
}

TEST_CASE("chorded modifier state matches exact chords") {
    app::ModifierState mods;
    mods.press(app::Modifier::Ctrl);
    mods.press(app::Modifier::Shift);

    CHECK(mods.isDown(app::Modifier::Ctrl));
    CHECK(mods.matches(app::Modifier::Ctrl | app::Modifier::Shift));
    CHECK_FALSE(mods.matches(static_cast<std::uint32_t>(app::Modifier::Ctrl)));

    mods.release(app::Modifier::Shift);
    CHECK(mods.matches(static_cast<std::uint32_t>(app::Modifier::Ctrl)));
}

TEST_CASE("hover state tracks target changes") {
    app::HoverState hover;
    CHECK_FALSE(hover.hovering());

    hover.update(Vec2{1.0f, 1.0f}, 7u);
    CHECK(hover.hovering());
    CHECK(hover.target() == 7u);
    CHECK(hover.targetChanged());

    hover.update(Vec2{1.5f, 1.5f}, 7u);
    CHECK_FALSE(hover.targetChanged());  // same target

    hover.update(Vec2{2.0f, 2.0f}, 9u);
    CHECK(hover.targetChanged());  // new target

    hover.clear();
    CHECK_FALSE(hover.hovering());
}

TEST_CASE("shortcut map resolves chords and honours rebinds") {
    app::ShortcutMap map;
    const std::uint32_t ctrl = static_cast<std::uint32_t>(app::Modifier::Ctrl);
    map.bind(app::Shortcut{ctrl, 'Z'}, "undo");
    map.bind(app::Shortcut{ctrl | app::Modifier::Shift, 'Z'}, "redo");

    CHECK(map.resolve(ctrl, 'Z') == "undo");
    CHECK(map.resolve(ctrl | app::Modifier::Shift, 'Z') == "redo");
    CHECK(map.resolve(0u, 'Z').empty());

    map.bind(app::Shortcut{ctrl, 'Z'}, "undo-v2");  // rebind wins
    CHECK(map.resolve(ctrl, 'Z') == "undo-v2");
}
