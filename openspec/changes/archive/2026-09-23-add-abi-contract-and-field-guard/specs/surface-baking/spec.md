# surface-baking — the field-evaluator boundary

## ADDED Requirements

### Requirement: A field evaluator's returns are validated at the boundary

A field evaluator is host-supplied code, so the bake SHALL validate what its
callbacks return rather than propagating it. The validation SHALL distinguish
two classes, separated by whether a CORRECT field can produce the value.

A CONTRACT VIOLATION — a NaN distance, a non-finite gradient, a non-finite
curvature, or an openness outside [0,1] beyond float tolerance — SHALL abandon
the whole bake and report which callback failed and where. It SHALL NOT be
sanitized into a usable value: a substituted default conceals the host's defect
and returns an image indistinguishable from a measured one.

A LEGITIMATELY UNDEFINED sample — an infinite distance, which is the ordinary
"nothing here" answer from a field covering a bounded region, or a zero-length
gradient, which is what a signed distance field has on its medial axis — SHALL
end the march for that texel only. That texel SHALL take the same neutral value
an un-hit cage ray produces, and such samples SHALL be counted on the result so
a host can see how much of its field the bake could not reach.

Callback out-params SHALL NOT be pre-seeded with a value that could pass
validation, so that a callback which returns without writing is detected rather
than read as a measurement.

#### Scenario: A broken callback fails the bake

- **WHEN** an evaluator returns a NaN distance, a non-finite gradient, or an
  openness far outside [0,1]
- **THEN** the bake SHALL fail, naming the callback, and SHALL produce no image

#### Scenario: An undefined sample is a counted miss

- **WHEN** an evaluator returns an infinite distance or a zero-length gradient
- **THEN** that texel SHALL take its neutral value, the sample SHALL be counted,
  and the bake SHALL succeed

#### Scenario: A well-behaved field is unaffected

- **WHEN** an evaluator honours its contract
- **THEN** the baked pixels SHALL be unchanged by the validation
