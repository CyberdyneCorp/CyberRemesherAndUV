# mesh-io (delta)

## MODIFIED Requirements

### Requirement: Loud failure semantics
Every import or export failure (missing file, parse error, unwritable destination, unsupported feature) SHALL produce a typed error with a human-readable message; the GUI SHALL surface it visibly and the CLI SHALL print it to stderr and exit nonzero. Silent failure is prohibited. A decoder SHALL treat every byte it did not write as hostile: no declared count, length or index from a file SHALL be trusted without being checked against the bytes actually present, allocation SHALL be bounded by what the payload can justify rather than by what it declares, and no input SHALL make a decoder loop without progress. A write SHALL NOT be reported successful until the bytes have reached the file.

#### Scenario: Unwritable export path
- **WHEN** an export targets a path that cannot be opened for writing
- **THEN** the operation SHALL fail with an explicit error naming the path and reason (AutoRemesher wrote nothing and reported nothing)

#### Scenario: Corrupt input file
- **WHEN** a malformed glTF file is imported
- **THEN** the importer SHALL reject it with a parse error and the current document SHALL remain unchanged

#### Scenario: A forged count or index is refused, not executed
- **WHEN** a file declares a section length, element count, list length or accessor/bufferView/buffer index that the payload cannot support — including a length chosen to overflow a bounds check (a section declaring 2^64−24 bytes), an out-of-range OBJ `vt`/`vn` index, a PLY element count larger than the remaining bytes, and a glTF accessor count exceeding its decoded buffer
- **THEN** the importer SHALL reject the file with a typed error, SHALL NOT read or write outside its own buffers, and SHALL NOT allocate on the declared figure

#### Scenario: Malformed input terminates in bounded time and memory
- **WHEN** a truncated, self-contradictory or adversarial file is imported — a PLY whose body ends mid-element, an element declaring zero properties with a nonzero count, or a compressed image stream whose expansion exceeds what its own header implies
- **THEN** the call SHALL return a typed error rather than hang or exhaust memory, and the peak allocation SHALL stay bounded by the size the file's own header can justify

#### Scenario: A write that fails at close is a failure
- **WHEN** a mesh, image or document write fills the disk or exceeds a quota, so the failure occurs when the buffer is flushed rather than when it is written
- **THEN** the call SHALL report failure naming the path, and SHALL NOT report success and lose the error in a destructor
