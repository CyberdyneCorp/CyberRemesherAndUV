// The map catalogue: the single table the C ABI's capability query, the CLI's
// --list-bake-maps and bake()'s own channel/field decisions all read.
//
// Before it existed the same facts lived in four switches. The risk a catalogue
// introduces is the opposite one: a table that says something the bake does not
// do. A consumer that sizes its buffer from an advertised channel count the bake
// disagrees with writes off the end of it, so the cases below check the
// catalogue against actual bakes rather than against itself.
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

TEST_CASE("every advertised map bakes, with the advertised channel count") {
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

TEST_CASE("fieldCapable names exactly the maps a field evaluator can answer alone") {
    // The narrowing a consumer holding a field and no Target has to see BEFORE
    // it asks. Checked against the behaviour rather than restated: a map marked
    // capable must bake from an empty Target, and one marked incapable must not.
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
