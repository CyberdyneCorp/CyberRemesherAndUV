# engine-bindings — ZRemesher surface

## ADDED Requirements

### Requirement: ZRemesher is reachable from every binding

The C ABI and the Python bindings SHALL expose the ZRemesher quad method and
its parameters — quality mode, adaptive sizing, local-feature-size
preservation, symmetry, guide mode — and SHALL expose the layout statistics and
quality score from the run report. Parity SHALL hold: any ZRemesher capability
reachable from the CLI SHALL be reachable from Python.

#### Scenario: Python drives the ZRemesher path

- **WHEN** a Python caller requests the ZRemesher quad method with a quality
  mode and a symmetry axis
- **THEN** the remesh SHALL run through that path and the returned report SHALL
  carry the layout statistics and the quality score

#### Scenario: Topology guides from Python

- **WHEN** a Python caller supplies a closed guide in topology mode
- **THEN** the binding SHALL forward the guide mode, and the report SHALL
  record the achieved guide adherence
