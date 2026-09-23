## Decisions

- `CyberIndexedMesh` is a borrowed input view: tightly packed finite XYZ
  positions, a `face_offsets` array of `face_count + 1` entries, and compact
  vertex indices. The call copies all accepted data before returning, so caller
  buffers can immediately be reused or mutated.
- Offsets are a CSR sequence: begin at zero, are monotonic, end at
  `index_count`, and each face has at least three corners. Polygon arity is
  preserved. Unused vertices are retained because host index identity is a
  meaningful import contract.
- Import validates null/count combinations, overflow-safe size relations,
  finite coordinates, offsets, index range and kernel topology before
  publishing a handle. It constructs a temporary mesh and only writes `*out`
  on success.
- Export uses query-then-copy: callers query position, offset and index counts
  independently, then provide exact-or-larger buffers. The C ABI never retains
  output pointers and returns the required count without partial logical
  records.

## Non-Goals

Borrowed/zero-copy geometry is not included: every input is copied before the
call returns. The first schema is deliberately small but complete for the core
attribute model: named `float`, `int32`, `float2`, `float3`, and `float4`
columns in vertex, face, and corner domains. Edge columns are excluded because
edge identity is rebuilt by topology operations. Python and Swift expose the
complete typed surface; Rust exposes geometry only and documents that boundary.
