# Sculpt handoff format — version 1.0

The *sculpt handoff* is the interchange this engine accepts as a retopology /
UV / bake **Target** when the surface comes out of a sculpting engine rather
than off disk as a generic mesh. It exists so the sculpt → retopo → UV → bake
story can be one pipeline without either side linking the other.

> **Status: defined unilaterally by this repository.**
> The handoff was specified here, and this repo ships the *reading* half only.
> Agreement with ClayCore's export-profile change — "one shared document, two
> implementations" — is **outstanding**; ClayCore is not present in this tree
> and no negotiation has taken place. Any producer that emits the profile below
> is a first-class citizen: nothing in the reader is ClayCore-specific, and the
> engine has **no build or link dependency on ClayCore**.

## Contents of a handoff

| Payload | Required | Where it lands |
| --- | --- | --- |
| Triangle positions + connectivity | yes | `cyber::Mesh` |
| Per-vertex normals | yes | vertex attribute `normal` (`Vec3`) |
| Per-vertex colors (polypaint) | yes | vertex attribute `color` (`Vec3`, 0..1) |
| Per-vertex material mix | yes | vertex attribute `material_mix` (`float`) |
| Producer label | no | `Handoff::producer` |

`material_mix` is the sculpt's per-vertex blend weight between two material
slots, in `[0, 1]`. The engine does not interpret it; it carries it so a bake
or an export can.

The vertex `normal` attribute is deliberately named the same as the engine's
per-**corner** `normal` attribute. They live in different attribute sets
(`Mesh::vertexAttributes()` vs `Mesh::cornerAttributes()`) so there is no
collision, but do not confuse the two when writing consumers.

## Version declaration and compatibility

Every handoff declares a `<major>.<minor>` version. This engine supports
**1.0**.

The compatibility rule is:

* **same major, minor ≤ supported** → accepted.
* **anything else** → rejected with `cyber::io::ErrorCode::IncompatibleVersion`
  and a message naming *both* the version found and the version supported.

A *newer minor* is rejected rather than read with the unknown parts dropped.
This matches the export-preset precedent in this repo (an unknown preset field
is an error, not a silent skip): silently ignoring a field the producer added
specifically to change the result is the failure mode the versioning exists to
prevent.

The version gate runs **before any geometry is read**. A handoff with a bad
version can never produce a partial Target, however malformed the rest of the
payload is.

## File profile: PLY

The PLY profile is the normative on-disk form. It is a standard PLY
(`ascii`, `binary_little_endian` or `binary_big_endian`) with a required
comment line and a required property set.

```ply
ply
format ascii 1.0
comment cyber_sculpt_handoff 1 0
comment cyber_handoff_producer claycore
element vertex 4
property float x
property float y
property float z
property float nx
property float ny
property float nz
property uchar red
property uchar green
property uchar blue
property float material_mix
element face 2
property list uchar int vertex_indices
end_header
...
```

* `comment cyber_sculpt_handoff <major> <minor>` — **required**. A PLY without
  it is not a handoff and is rejected with `ErrorCode::UnsupportedFormat`
  (import it as a plain mesh instead, via `cyber::io::importMesh`).
* `comment cyber_handoff_producer <label>` — optional free-form producer name,
  surfaced in the CLI report.
* Vertex properties `x y z`, `nx ny nz`, `red green blue`, `material_mix` are
  all required. `red/green/blue` are `uchar` 0..255 and are divided by 255.
* Faces must be **triangles**. A face list entry with any other arity is
  rejected: a sculpt export that is not triangulated is a producer bug, and
  triangulating it here would change what the bake sees.

## File profile: glTF / GLB

A `.gltf`/`.glb` handoff declares its version in
`asset.extras.cyberSculptHandoff`:

```json
{ "asset": { "extras": { "cyberSculptHandoff": { "major": 1, "minor": 0,
                                                 "producer": "claycore" } } } }
```

Geometry, normals and vertex colors come through the engine's normal glTF
import path. `material_mix` has no glTF core slot; a GLB handoff that omits it
is accepted with a warning rather than rejected, since the attribute is the one
payload the format cannot carry natively.

## In-memory buffer profile

For on-device composition (both engines in one process), the same handoff
arrives as plain arrays with no intermediate file — `cyber::handoff::BufferView`:

| Field | Meaning |
| --- | --- |
| `positions` | `3 * vertexCount` floats, xyz interleaved (required) |
| `normals` | `3 * vertexCount` floats (may be null) |
| `colors` | `3 * vertexCount` floats in `[0,1]` (may be null) |
| `materialMix` | `vertexCount` floats (may be null) |
| `indices` | `indexCount` `uint32`, triangles, `indexCount % 3 == 0` |
| `version` | the declared version — gated by the same rule as a file |
| `producer` | optional NUL-terminated label |

The buffer route produces a `Handoff` that is equivalent to the file route for
the same data: a mesh read from a buffer and the same mesh written to a PLY and
read back carry the same positions, connectivity and attribute columns.

## Reading a handoff

```cpp
#include "cyber/handoff/handoff.hpp"

const auto result = cyber::handoff::readFile("sculpt.ply");
if (!result.ok()) {
    // result.error().code == cyber::io::ErrorCode::IncompatibleVersion
    // result.error().message names both versions.
    return;
}
const cyber::Mesh& target = result.value().mesh;
```

`readStream(std::istream&, originLabel)` reads the same profile from stdin or
any other stream; `readBuffers(const BufferView&)` reads the in-memory profile.

From the CLI:

```sh
cyberremesh --target sculpt.ply --output low.obj \
            --preset blender --bake normal,ao,curvature --report run.json
# or, piped from a producer:
producer --for-retopo | cyberremesh --target - --output low.obj --preset blender
```

`run.json` records the handoff under a `handoff` block (version, producer,
source, vertex count, which optional payloads were present).

## Versioning policy

* **Minor bump** — a new optional payload that a 1.x reader could ignore
  safely *if it chose to*. Readers still reject newer minors; the bump exists
  so the rejection message is accurate.
* **Major bump** — any change to the meaning of an existing payload, or a new
  required one.

`cyber::handoff::kVersionMajor` / `kVersionMinor` are the single source of
truth in this repo.
