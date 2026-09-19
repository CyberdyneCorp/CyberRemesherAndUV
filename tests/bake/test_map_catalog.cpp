// The map catalogue: the single table the C ABI's capability query, the CLI's
// --list-bake-maps and bake()'s own channel/field decisions all read.
//
// Before it existed the same facts lived in four switches. The risk a catalogue
// introduces is the opposite one: a table that says something the bake does not
// do. A consumer that sizes its buffer from an advertised channel count the bake
// disagrees with writes off the end of it.
//
// Which is why the FIRST case below restates the whole table by hand. bake()
// now takes its channel count and its field support FROM the catalogue, so a
// case that bakes a map and compares the result against the same table the bake
// read asserts nothing -- both sides move together, and a mistyped row would be
// advertised to a consumer AND baked, consistently and wrongly. The
// hand-written copy is the only independent statement of what this build
// promises, so editing the table means editing that case too, deliberately.
//
// The cases after it check what the table does not decide by itself: the
// encoding basis a bake reports (encodingFor() still owns that), the colour
// space preset parsing defaults a map to, the export-preset name vocabulary,
// and that each row does describe a map that really bakes.
#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/bake/field_evaluator.hpp"
#include "cyber/bake/map_catalog.hpp"
#include "cyber/core/export_preset.hpp"
#include "cyber/core/mesh.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
using cyber::bake::bake;
using cyber::bake::BakeMap;
using cyber::bake::BakeParams;
using cyber::bake::BakeResult;
using cyber::bake::EncodingBasis;
using cyber::bake::findMap;
using cyber::bake::mapCatalog;
using cyber::bake::mapCatalogNames;
using cyber::bake::MapInfo;
using cyber::bake::NormalSpace;

namespace {

// The two-triangle unit plane with per-corner UVs covering the whole layout,
// usable as both the low-poly and the coincident Target.
Mesh uvPlane() {
    const std::vector<Vec3> positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const std::vector<std::vector<Index>> faces = {{0, 1, 2, 3}};
    Mesh mesh = Mesh::fromIndexed(positions, faces);
    auto& uv = mesh.cornerAttributes().create<Vec2>("uv");
    for (Index face = 0; face < mesh.faceCapacity(); ++face) {
        if (!mesh.isAlive(FaceId{face})) {
            continue;
        }
        for (const LoopId loop : mesh.faceLoops(FaceId{face})) {
            const Vec3 position = mesh.position(mesh.loopVertex(loop));
            uv[loop.value] = {position.x, position.y};
        }
    }
    return mesh;
}

}  // namespace

TEST_CASE("the advertised catalogue is pinned, row by row") {
    // Restated by hand, from the requirement rather than from the table -- see
    // the file header for why a bake cannot check these two columns any more.
    // A row changed here without a reason is a consumer told the wrong channel
    // count (it sizes its buffer from it) or the wrong colour space (it applies
    // a transfer curve from it).
    struct Advertised {
        BakeMap map;
        std::string_view name;
        int channels;
        EncodingBasis basis;
        bool srgb;
        bool fieldCapable;
    };
    const std::vector<Advertised> expected = {
        {BakeMap::Normal, "normal", 3, EncodingBasis::TangentNormal, false, true},
        {BakeMap::AmbientOcclusion, "ao", 1, EncodingBasis::None, false, true},
        {BakeMap::Displacement, "displacement", 1, EncodingBasis::Distance, false, false},
        {BakeMap::Position, "position", 3, EncodingBasis::None, false, false},
        {BakeMap::Color, "color", 3, EncodingBasis::None, true, false},
        {BakeMap::Curvature, "curvature", 1, EncodingBasis::None, false, true},
        {BakeMap::Cavity, "cavity", 1, EncodingBasis::None, false, true},
        {BakeMap::ObjectNormal, "object-normal", 3, EncodingBasis::ObjectNormal, false, false},
        {BakeMap::ObjectPosition, "object-position", 3, EncodingBasis::ObjectBounds, false, false},
        {BakeMap::BentNormal, "bent-normal", 3, EncodingBasis::TangentNormal, false, false},
        {BakeMap::Thickness, "thickness", 1, EncodingBasis::Distance, false, false},
        {BakeMap::MaterialId, "material-id", 3, EncodingBasis::IdColor, false, false},
        {BakeMap::ObjectId, "object-id", 3, EncodingBasis::IdColor, false, false},
    };

    const std::span<const MapInfo> catalog = mapCatalog();
    // Not just the count the static_assert already pins: the ORDER too, because
    // findMap(BakeMap) indexes the table by enumerator.
    REQUIRE(catalog.size() == expected.size());
    for (std::size_t row = 0; row < expected.size(); ++row) {
        const Advertised& want = expected[row];
        const MapInfo& got = catalog[row];
        CAPTURE(std::string(want.name));
        CHECK(got.map == want.map);
        CHECK(got.name == want.name);
        CHECK(got.channels == want.channels);
        CHECK(got.basis == want.basis);
        CHECK(got.srgb == want.srgb);
        CHECK(got.fieldCapable == want.fieldCapable);
    }
}

TEST_CASE("the catalogue advertises every map exactly once, with unique names") {
    const std::span<const MapInfo> catalog = mapCatalog();
    REQUIRE(!catalog.empty());

    std::set<int> codes;
    std::set<std::string> names;
    for (const MapInfo& info : catalog) {
        CHECK(codes.insert(static_cast<int>(info.map)).second);
        CHECK(names.insert(std::string(info.name)).second);
        CHECK(!info.name.empty());
        // A consumer reads this to size a buffer; anything else is a crash.
        CHECK((info.channels == 1 || info.channels == 3));
    }
    CHECK(codes.size() == catalog.size());

    // The table is indexed by enumerator, so a row in the wrong slot would make
    // findMap() hand back the neighbouring map's channel count.
    for (const MapInfo& info : catalog) {
        const MapInfo* byCode = findMap(info.map);
        REQUIRE(byCode != nullptr);
        CHECK(byCode->map == info.map);
        const MapInfo* byName = findMap(info.name);
        REQUIRE(byName != nullptr);
        CHECK(byName->map == info.map);
    }
    CHECK(findMap(std::string_view("not-a-map")) == nullptr);
}

TEST_CASE("every advertised row describes a map that really bakes, at the size it states") {
    // What the pinned table cannot say: that the row belongs to a map this
    // build can actually produce, and that the buffer length a consumer
    // computes from width * height * channels is the length the bake fills.
    // The channel count itself is pinned above; bake() reads it from here.
    const Mesh low = uvPlane();
    const Mesh high = uvPlane();
    BakeParams params;
    params.width = 8;
    params.height = 8;

    for (const MapInfo& info : mapCatalog()) {
        CAPTURE(std::string(info.name));
        const BakeResult result = bake(low, high, info.map, params);
        REQUIRE(!result.image.pixels.empty());
        CHECK(result.image.channels == info.channels);
        CHECK(result.image.pixels.size() ==
              static_cast<std::size_t>(params.width * params.height * info.channels));
    }
}

TEST_CASE("the advertised encoding basis is what a default-parameter bake reports") {
    const Mesh low = uvPlane();
    const Mesh high = uvPlane();
    BakeParams params;  // DEFAULT parameters: the qualifier the catalogue states
    params.width = 8;
    params.height = 8;

    for (const MapInfo& info : mapCatalog()) {
        CAPTURE(std::string(info.name));
        const BakeResult result = bake(low, high, info.map, params);
        CHECK(result.encoding.basis == info.basis);
    }

    // And the documented exception: BentNormal's basis follows the parameter, so
    // the catalogue's row is a default and not an authority. If this ever stops
    // being true the catalogue can state a basis outright.
    params.bentNormalSpace = NormalSpace::Object;
    const BakeResult objectSpace = bake(low, high, BakeMap::BentNormal, params);
    CHECK(objectSpace.encoding.basis == EncodingBasis::ObjectNormal);
    CHECK(findMap(BakeMap::BentNormal)->basis == EncodingBasis::TangentNormal);
}

TEST_CASE("fieldCapable is the narrowing bake() actually applies without a Target") {
    // The narrowing a consumer holding a field and no Target has to see BEFORE
    // it asks. fieldSupports() reads this same column, so what this case pins is
    // the WIRING -- that the column reaches the refusal at all, and reaches it
    // for every map rather than for the four the old switch happened to list.
    // Which maps belong in the column is pinned by hand at the top of the file.
    const Mesh low = uvPlane();
    const Mesh empty;
    BakeParams params;
    params.width = 4;
    params.height = 4;

    struct FlatField final : cyber::bake::FieldEvaluator {
        [[nodiscard]] float distance(Vec3 p) const override { return p.z; }
        [[nodiscard]] Vec3 gradient(Vec3) const override { return Vec3{0, 0, 1}; }
        [[nodiscard]] float openness(Vec3, Vec3, float) const override { return 1.0f; }
    };
    const FlatField field;
    params.field = &field;

    for (const MapInfo& info : mapCatalog()) {
        CAPTURE(std::string(info.name));
        const BakeResult result = bake(low, empty, info.map, params);
        CHECK(result.image.pixels.empty() != info.fieldCapable);
    }
}

TEST_CASE("the catalogue's names are mesh-io's export-preset vocabulary") {
    // Two lists a consumer is expected to join: the maps a named preset declares
    // and the maps the provider advertises. They must be the same spellings, or
    // the consumer is left writing a translation table nobody maintains.
    for (const MapInfo& info : mapCatalog()) {
        CAPTURE(std::string(info.name));
        const auto preset = cyber::io::presetMapFromName(std::string(info.name));
        REQUIRE(preset.has_value());
        CHECK(cyber::io::presetMapName(*preset) == info.name);
    }
}

TEST_CASE("the name list is a diagnostic a caller can read") {
    const std::string all = mapCatalogNames();
    const std::string fieldOnly = mapCatalogNames(true);
    for (const MapInfo& info : mapCatalog()) {
        CHECK(all.find(std::string(info.name)) != std::string::npos);
        CHECK((fieldOnly.find(std::string(info.name)) != std::string::npos) == info.fieldCapable);
    }
    CHECK(fieldOnly.size() < all.size());
}

TEST_CASE("the advertised colour space is the one preset parsing defaults that map to") {
    // "Colour is appearance, every other map is data" is stated in four places:
    // the built-in presets, the CLI's --bake override, preset parsing's default
    // for an under-specified map, and MapInfo::srgb. The consumer that joins a
    // preset's map list to the advertised set reads two of them, and a drift
    // between those two either gamma-encodes data or ships appearance flat.
    // This is the only cross-check available for the column, since the bake
    // itself never looks at it.
    for (const MapInfo& info : mapCatalog()) {
        CAPTURE(std::string(info.name));
        const std::string json = R"({"schemaVersion":)" +
                                 std::to_string(cyber::io::kPresetSchemaVersion) +
                                 R"(,"name":"t","maps":[")" + std::string(info.name) + R"("]})";
        const auto parsed = cyber::io::parsePreset(json);
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value().maps.size() == 1u);
        const bool presetSrgb = parsed.value().maps[0].colorSpace == cyber::io::ColorSpace::Srgb;
        CHECK(presetSrgb == info.srgb);
    }
}
