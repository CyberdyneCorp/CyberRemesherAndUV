# engine-bindings — element-id staleness and callback naming

## ADDED Requirements

### Requirement: Element-id staleness is detectable, not only documented

The ABI SHALL expose a monotone counter per mesh handle that changes whenever
that handle's element ids may have been reassigned, so that a host holding
id-keyed state can verify it rather than infer from documentation which
operations preserve ids.

The counter SHALL follow the same distinction the element-id rules draw: an
operation that only moves vertex positions SHALL NOT change it, and an operation
that may create, destroy or rewire elements SHALL change it. It SHALL be a hint
in the SAFE direction only — it may change when ids in fact survived, never the
reverse — so equal values prove ids are valid while differing values only
require the host not to assume. A clone SHALL carry its source's value, because
a clone's ids are the source's ids.

#### Scenario: A positions-only edit preserves the counter

- **WHEN** an operation moves vertex positions without changing topology
- **THEN** the counter SHALL be unchanged, so caller-side annotations survive

#### Scenario: A rebuild changes the counter

- **WHEN** an operation reassigns element ids, such as subdivision
- **THEN** the counter SHALL change, and SHALL never return to an earlier value

### Requirement: A callback is named for the value it returns

Where the engine asks a host for a quantity through a callback, the override
point SHALL be named for what it RETURNS, not for the quantity's inverse or a
related term, so that an implementer following the name computes the value the
engine consumes. Where a name is frozen by the ABI and cannot be corrected, the
polarity SHALL be stated in the documentation a consumer reads when choosing the
operation, not only at the callback's own declaration.

#### Scenario: An implementer following the name is correct

- **WHEN** a host implements the ambient-occlusion callback from its name alone
- **THEN** the value it computes SHALL be the one the bake consumes, so the
  resulting map is not inverted
