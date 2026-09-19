#include "cyber/bake/map_catalog.hpp"

#include <array>

namespace cyber::bake {

namespace {

// One row per BakeMap enumerator, in enumerator order. The static_assert below
// pins the count, so adding a map without adding its row does not compile.
//
// `basis` is the basis under DEFAULT parameters (see MapInfo). BentNormal's row
// says Tangent because BakeParams::bentNormalSpace defaults to Tangent; a bake in
// object space reports ObjectNormal, and BakeResult::encoding is what says so.
constexpr std::array<MapInfo, 13> kCatalog{{
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
}};

// The last enumerator plus one. A map appended to BakeMap without a catalogue row
// would otherwise be advertised as absent by every entry point that reads the
// table -- silently, which is the failure this whole file exists to prevent.
static_assert(kCatalog.size() == static_cast<std::size_t>(BakeMap::ObjectId) + 1,
              "every BakeMap enumerator needs a catalogue row, in enumerator order");

}  // namespace

std::span<const MapInfo> mapCatalog() { return std::span<const MapInfo>(kCatalog); }

const MapInfo* findMap(BakeMap map) {
    // Indexed rather than searched, which the static_assert above makes safe: the
    // table is in enumerator order and covers every enumerator.
    const auto index = static_cast<std::size_t>(map);
    if (index >= kCatalog.size()) {
        return nullptr;
    }
    return &kCatalog[index];
}

const MapInfo* findMap(std::string_view name) {
    for (const MapInfo& info : kCatalog) {
        if (info.name == name) {
            return &info;
        }
    }
    return nullptr;
}

std::string mapCatalogNames(bool fieldOnly) {
    std::string out;
    for (const MapInfo& info : kCatalog) {
        if (fieldOnly && !info.fieldCapable) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += info.name;
    }
    return out;
}

}  // namespace cyber::bake
