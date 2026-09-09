# engine-bindings — the ABI version contract

## ADDED Requirements

### Requirement: The ABI carries its own version, distinct from the engine's

This elaborates the one-line promise already in "Full-surface C ABI facade"
("The ABI SHALL carry a runtime-queryable semantic version; minor releases SHALL
be additive only") into a contract with testable scenarios. That sentence had no
implementation: the only version an embedder could read was the ENGINE's, which
moves for quality work that changes no declaration.

The ABI version SHALL be distinct from the engine's semantic version and SHALL
be declared in the public header as preprocessor constants, so that a consumer
vendoring the source — or generating bindings from the header — obtains it
without reading the build system. It SHALL be major.minor with no patch
component, because no change to the linkable surface is neither additive nor
breaking.

A library SHALL serve a client when the majors are equal and the library's minor
is at least the client's. That rule SHALL be implemented once, inside the
engine, and exposed as an entry point, so that every binding reaches the same
verdict rather than reimplementing the comparison. A mismatch SHALL be reported
as a status code with both versions retrievable; the library SHALL NOT abort,
exit or log, because it runs inside a host process.

The shared library's SONAME SHALL carry the ABI major, not the project major.

Appending an enumerator to an existing enum SHALL NOT be treated as an additive
minor change: an unfixed C enum's value range is inferred from its enumerators,
so handing a client a value outside the range it compiled against is undefined
on the client's side.

#### Scenario: ABI version query

- **WHEN** a client compiled against ABI 1.x loads a 1.y (y > x) library
- **THEN** all 1.x entry points SHALL work unchanged, and the compatibility
  check SHALL report success

#### Scenario: A newer client is refused by an older library

- **WHEN** a client compiled against a LATER ABI minor than the library
  implements asks the library to serve it
- **THEN** the check SHALL fail with both versions named, because the entry
  points the client compiled against are genuinely absent

#### Scenario: The two versions are independent

- **WHEN** the engine's behaviour changes without altering any declaration
- **THEN** the engine version SHALL move and the ABI version SHALL NOT, and a
  matching ABI SHALL NOT be taken as a promise of identical output
