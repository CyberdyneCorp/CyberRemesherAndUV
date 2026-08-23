# compute-acceleration — a host-settable worker-thread budget

Delta for `mobile-targets-buildable`.

## ADDED Requirements

### Requirement: Host-bounded worker fan-out
The library SHALL let the embedding host cap the worker threads any one
parallel loop may fan out to, and every parallel loop in the engine SHALL
honour that cap. Unset, the loops SHALL keep sizing themselves from the
machine's hardware concurrency, so the default behaviour is unchanged.

The control SHALL be an API call, not an environment variable: a DCC or app
host learns its thread budget while it is already running and multi-threaded,
where `setenv` is not safe, and an interactive host needs to move the budget
between operations rather than once at startup.

The cap SHALL NOT change any result. The loops split a range into contiguous
chunks and each chunk writes only its own indices, so a capped run SHALL be
byte-identical to an uncapped one; the cap buys CPU headroom and nothing else.
This SHALL be verified by a test comparing whole outputs, not by inspection.

Where the library reports the active device's thread count, it SHALL report the
fan-out a loop would actually get, so a host that lowered the cap does not read
back the machine's core count.

#### Scenario: A capped bake leaves the host cores to spare
- **WHEN** a host caps the worker threads and then runs a bake
- **THEN** the engine's parallel loops SHALL fan out to at most that many workers

#### Scenario: The cap cannot alter output
- **WHEN** the same remesh runs capped and uncapped
- **THEN** the two results SHALL be identical vertex for vertex and face for face
