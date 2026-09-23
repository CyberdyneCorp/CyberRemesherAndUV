## ADDED Requirements

### Requirement: Bulk indexed polygon exchange

The C ABI SHALL import a mesh from a copying packed-XYZ and CSR polygon
descriptor and SHALL export authored polygon offsets and indices without
triangulating them. The import SHALL reject malformed counts, non-finite
coordinates, non-monotonic offsets, faces with fewer than three corners, and
out-of-range indices without publishing a partial output handle.

#### Scenario: Mixed polygon round trip

- **WHEN** a host imports shared-vertex triangles, quads, and n-gons
- **THEN** authored polygon arities and connectivity SHALL be returned by the
  bulk export

#### Scenario: Invalid descriptor

- **WHEN** a host supplies invalid offsets, indices, or coordinates
- **THEN** import SHALL fail and leave its output pointer unchanged
