## ADDED Requirements

### Requirement: The bake provider is a seam, not a dependency

The provider surface through which an external consumer requests baked maps SHALL be an
INTERFACE and a set of result records, exactly as the field evaluator is. A consumer SHALL
drive it with nothing but this repository's C ABI: no build dependency, no link dependency
and no source dependency SHALL exist in either direction between this engine and any
consumer of its maps. This repository SHALL carry a headless example that drives the whole
provider surface — enumerating the maps, requesting them, reading the metadata, observing
progress and cancelling a bake — so the seam is exercised here without any consumer
present.

The provider SHALL accept the same field evaluator the bridge already defines. When an
evaluator is supplied in place of a Target mesh, the producible set SHALL be exactly the
maps a field can answer, and a request for any other map SHALL fail naming that map and
listing the producible set — the same refusal a map outside the advertised set gets,
because to that consumer it is outside the advertised set. Which maps a field alone can
produce SHALL be readable from the capability query before a request is made, not only
from a refusal afterwards.

#### Scenario: A consumer drives the provider over the C ABI alone

- **WHEN** a consumer compiled against this repository's C header, and linked against
  nothing else of this repository, enumerates the producible maps and requests one
- **THEN** it SHALL receive the pixels and the result metadata, and this repository SHALL
  require no artifact of that consumer to build, test or run

#### Scenario: The example exercises the surface headlessly

- **WHEN** this repository's bake-provider example runs with no display and no consumer
  present
- **THEN** it SHALL enumerate the advertised maps, request maps of more than one encoding
  basis, print the metadata each one reported, show a refusal naming an unproducible map,
  and show a cancelled request returning no pixels

#### Scenario: A field evaluator narrows the producible set, and says so in advance

- **WHEN** a consumer supplies a field evaluator instead of a Target mesh
- **THEN** the capability query SHALL already mark which maps a field alone can produce,
  a request for one of those SHALL succeed, and a request for any other SHALL fail naming
  that map and listing the maps a field can produce
