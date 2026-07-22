#include <doctest.h>

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

Mesh makeTriMesh() {
    const std::vector<Vec3> positions = {
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
    const std::vector<std::vector<Index>> faces = {{0, 1, 2}};
    return Mesh::fromIndexed(positions, faces);
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
