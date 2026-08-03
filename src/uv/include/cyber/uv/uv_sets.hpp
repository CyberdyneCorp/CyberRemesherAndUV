#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"

// Multiple UV sets per mesh (uv-workflow spec, "UDIMs and multiple UV sets").
//
// The design that keeps this cheap: the ACTIVE set always lives under the ordinary `uv` corner
// attribute, and every inactive set is stored beside it as `uv:<name>`. So the entire UV module —
// atlas, packing, distortion, transforms, stacking, everything — needs no change whatsoever and
// cannot accidentally read the wrong set. Switching sets swaps two columns.
//
// The active set's NAME is not stored in the mesh, because `AttributeSet` holds numeric columns
// only. Callers pass it explicitly; the C layer keeps it on the mesh handle. That also keeps these
// functions pure enough to test by passing a name in.
namespace cyber::uv {

// Prefix marking a stored (inactive) UV set column.
inline constexpr std::string_view kUvSetPrefix = "uv:";

// The default active set's name, used when a caller has never named one.
inline constexpr std::string_view kDefaultUvSetName = "default";

// Names of every set on the mesh, ascending, including `activeName` when the mesh has UVs.
//
// Ascending so a serialized set list is deterministic — a map's order is an implementation detail
// that would make the same document save differently run to run.
[[nodiscard]] std::vector<std::string> uvSetNames(const Mesh& mesh, const std::string& activeName);

// True when `name` exists, active or stored.
[[nodiscard]] bool hasUvSet(const Mesh& mesh, const std::string& activeName,
                            const std::string& name);

// Creates `name` as a COPY of the active set, leaving the active set active.
//
// A copy rather than an empty set: a new UV set almost always starts as a variant of the layout
// you already have, and an empty set would read downstream as a real layout collapsed at the
// origin — the absence-versus-zero trap the whole UV path avoids.
//
// Returns false when the mesh has no UVs at all (nothing to copy), or when `name` is empty, is
// already taken, or contains the ':' separator.
[[nodiscard]] bool createUvSet(Mesh& mesh, const std::string& activeName,
                               const std::string& name);

// Makes `name` the active set, storing the previously active one under its own name.
//
// Returns false when `name` does not exist or is already active. On success the caller MUST record
// `name` as the new active name — the mesh itself does not remember it.
[[nodiscard]] bool activateUvSet(Mesh& mesh, const std::string& activeName,
                                 const std::string& name);

// Deletes a STORED set. Refuses to delete the active one: that would leave the mesh with a `uv`
// column belonging to a set the caller still names, or no active set at all.
[[nodiscard]] bool deleteUvSet(Mesh& mesh, const std::string& activeName,
                               const std::string& name);

// Renames a set, active or stored. Returns false for the same validation reasons as `createUvSet`.
[[nodiscard]] bool renameUvSet(Mesh& mesh, const std::string& activeName,
                               const std::string& from, const std::string& to);

// MARK: - Serialization (the document sidecar)

// Serializes the active set's NAME and the data of every INACTIVE set.
//
// The active set's data is deliberately NOT stored. The document payload already carries it (as
// OBJ `vt`), and a second copy is a second source of truth: the first version of this DID store it,
// and on load the sidecar's stale copy overwrote the live layout — silently discarding every UV
// edit made since the sidecar was last written. A test caught it. The payload owns the active set;
// the sidecar owns the others plus the active set's name.
//
// Layout is little-endian and versioned; the corner count is recorded so a sidecar written for a
// different topology is rejected rather than silently misapplied.
[[nodiscard]] std::vector<std::uint8_t> serializeUvSets(const Mesh& mesh,
                                                        const std::string& activeName);

// Restores the INACTIVE sets from `bytes`, returning the active set's name, or an empty string on
// failure.
//
// NEVER touches the active `uv` column: the payload it came from is the authority for that layout,
// and overwriting it here would discard any edit made since the sidecar was written.
//
// Rejected — leaving the mesh untouched — when the blob is truncated, carries an unknown version,
// or was written for a different corner count. A UV set applied to the wrong topology would shear
// every island, which looks plausible and is wrong.
[[nodiscard]] std::string deserializeUvSets(Mesh& mesh, const std::vector<std::uint8_t>& bytes);

}  // namespace cyber::uv
