# mesh-io — an opt-in resource ceiling

## ADDED Requirements

### Requirement: A host can bound the size of a mesh it will import

Import SHALL accept an optional ceiling on the vertex count it will return, and
SHALL refuse a file exceeding it with a typed error naming both the file's count
and the ceiling.

The ceiling SHALL be a RESOURCE bound and SHALL be documented as distinct from
the structural checks that refuse malformed input: a declared element count is
already validated against the bytes a file carries, which needs no configured
number, and conflating the two yields a limit fit for neither purpose.

It SHALL default to disabled, because the engine cannot know a host's budget and
any value chosen for it would be wrong on some supported device. What the
ceiling bounds SHALL be stated honestly: the returned mesh, not the peak parse
allocation.

#### Scenario: A legitimate file over the host's budget is refused

- **WHEN** a well-formed file carrying more vertices than the ceiling is imported
- **THEN** the import SHALL fail with an error naming the count and the ceiling,
  and SHALL NOT return a mesh

#### Scenario: The ceiling is off by default

- **WHEN** no ceiling has been set
- **THEN** import SHALL behave as it did before the ceiling existed
