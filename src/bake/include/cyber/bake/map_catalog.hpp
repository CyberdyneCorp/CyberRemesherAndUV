#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "cyber/bake/bake.hpp"

// The set of maps this build produces, as DATA rather than as a switch repeated
// in every entry point (surface-baking spec, "Bakeable map types"; engine-bindings
// spec, "Bake provider surface for an external map consumer").
//
// Before this table existed the map list lived in four places: `channelsFor` and
// `fieldSupports` in bake.cpp, `toBakeMap` in the C ABI, and the CLI's `--bake`
// help text and parser. The capability query a consumer TRUSTS would have been a
// fifth. One table, three readers.
namespace cyber::bake {

// What a consumer needs in order to decide whether it wants a map, and to size
// the buffer it will be written into, WITHOUT baking it first.
struct MapInfo {
    BakeMap map = BakeMap::Normal;
    // Stable machine name. Deliberately mesh-io's export-preset vocabulary
    // ("normal", "ao", "object-position", "material-id", ...) rather than a new
    // spelling: a consumer joining a named preset's map list to the advertised
    // set must not have to translate between two of them.
    std::string_view name;
    int channels = 3;  // floats per texel
    // The basis a bake of this map reports UNDER DEFAULT PARAMETERS. It is not
    // the authority: BakeMap::BentNormal reports ObjectNormal or TangentNormal
    // depending on BakeParams::bentNormalSpace, so a static table cannot state
    // one basis for it and be right for both. BakeResult::encoding, filled per
    // bake, is the authority.
    EncodingBasis basis = EncodingBasis::None;
    // True when the texels carry APPEARANCE and want a transfer curve on the way
    // to an 8-bit file; false when they carry DATA and a gamma curve on them is a
    // bug in every target app. An IdColor map is data of the strictest kind --
    // exact keys -- and is never colour-converted whatever this says.
    bool srgb = false;
    // True when a FieldEvaluator alone can produce this map, so a consumer that
    // has a field and no Target mesh can see the narrowed set BEFORE it asks
    // rather than only from the refusal afterwards.
    bool fieldCapable = false;
};

// Every producible map, in a stable order (the BakeMap enumerator order, which is
// the order the maps were added). Never empty.
[[nodiscard]] std::span<const MapInfo> mapCatalog();

// The catalogue entry for `map`, or nullptr when `map` is not one of the
// advertised maps -- which, for a value that arrived across a C boundary, is the
// ordinary case rather than an impossible one.
[[nodiscard]] const MapInfo* findMap(BakeMap map);

// The catalogue entry whose `name` matches exactly, or nullptr.
[[nodiscard]] const MapInfo* findMap(std::string_view name);

// The advertised names, comma-separated, for a diagnostic that has to tell a
// caller what it could have asked for. `fieldOnly` restricts the list to the maps
// a field evaluator alone can produce.
[[nodiscard]] std::string mapCatalogNames(bool fieldOnly = false);

}  // namespace cyber::bake
