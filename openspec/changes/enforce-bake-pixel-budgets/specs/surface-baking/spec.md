## ADDED Requirements

### Requirement: A host can cap bake texel allocation

The system SHALL allow a host to configure an optional maximum number of bake
texels. It SHALL reject a request whose `width * height` exceeds that ceiling
before UV rasterization or output image allocation. Zero SHALL disable the
ceiling, and an overflowed texel product SHALL be rejected.

#### Scenario: Requested bake is over budget

- **GIVEN** a host sets a texel ceiling below the requested width times height
- **WHEN** it starts a bake
- **THEN** the operation SHALL fail with a diagnostic naming the request and ceiling
- **AND** no output image SHALL be returned
